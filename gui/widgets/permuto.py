"""Permuto page: create the trading identity, back it up, register, verify.

Four states, and the page shows exactly one:

1. **No identity** -- offer to create it, or restore from 24 words.
2. **Identity, backup not confirmed** -- refuse to register. The words are the
   only thing standing between the operator and a lost account, and the moment
   after creation is the only moment they can be written down.
3. **Backed up, not registered** -- the Register button is live.
4. **Registered** -- green confirmation, with standing read back from the
   leaderboard rather than merely asserted.

WHY REGISTRATION IS GATED ON THE BACKUP CHECKBOX.  Registering binds this key
to the exchange account permanently: there is no "change my key" flow, and on
this venue the key IS the account. An operator who registers first and backs
up later has a window in which a disk failure costs them the entry -- and
during the contest, the standing too. So the gate is deliberate friction, not
ceremony.

Network calls run on a worker thread. The identity's private key never crosses
a signal boundary: the worker holds the identity object and hands out only the
public result.
"""

from __future__ import annotations

import hashlib
import logging
from typing import Any, Optional

from PySide6.QtCore import QObject, Qt, QThread, Signal, Slot
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QDialog,
    QDialogButtonBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS as _C

_log = logging.getLogger(__name__)

PERMUTO_URL = "https://perps.permuto.capital"


# --------------------------------------------------------------------------- #
# Recovery-phrase modal
# --------------------------------------------------------------------------- #

class _RecoveryPhraseDialog(QDialog):
    """Show the 24 words once, and make the operator say they wrote them down.

    Shown exactly once, at creation. The phrase is not stored, so there is no
    "show it again" -- that is the point of holding only a wrapped key, and the
    dialog says so rather than letting the operator discover it later.
    """

    def __init__(self, phrase: str, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Permuto recovery phrase")
        self.setModal(True)
        self.setMinimumWidth(560)
        self._copied_digest: Optional[bytes] = None

        layout = QVBoxLayout(self)

        title = QLabel("Write these 24 words down now")
        title.setStyleSheet(
            f"color: {_C.TEXT_PRIMARY}; font-size: 16px; font-weight: bold;"
        )
        layout.addWidget(title)

        warning = QLabel(
            "This is the ONLY copy. It is not saved anywhere and cannot be "
            "shown again.\n\nThese words are your Permuto account. Anyone who "
            "has them controls it; if you lose them and this machine, the "
            "account is gone.\n\nPaper or metal, stored off this computer. "
            "Not a screenshot, not a text file."
        )
        warning.setWordWrap(True)
        warning.setStyleSheet(f"color: {_C.WARNING_YELLOW};")
        layout.addWidget(warning)

        words = phrase.split()
        grid = QTextEdit()
        grid.setReadOnly(True)
        grid.setPlainText(
            "\n".join(
                "   ".join(
                    "%2d. %-9s" % (i + n + 1, words[i + n])
                    for n in range(4)
                    if i + n < len(words)
                )
                for i in range(0, len(words), 4)
            )
        )
        grid.setStyleSheet(
            f"background: {_C.ELEVATED_BG}; color: {_C.TEXT_PRIMARY}; "
            f"border: 1px solid {_C.BORDER}; font-family: monospace;"
        )
        grid.setFixedHeight(160)
        layout.addWidget(grid)

        copy_row = QHBoxLayout()
        copy_btn = QPushButton("Copy to clipboard")
        copy_btn.clicked.connect(lambda: self._copy(phrase))
        copy_row.addWidget(copy_btn)
        self._copy_notice = QLabel("")
        self._copy_notice.setStyleSheet(f"color: {_C.INFO_BLUE};")
        copy_row.addWidget(self._copy_notice)
        copy_row.addStretch(1)
        layout.addLayout(copy_row)

        self._confirm = QCheckBox(
            "I have written down all 24 words and stored them safely"
        )
        self._confirm.setStyleSheet(f"color: {_C.TEXT_PRIMARY};")
        layout.addWidget(self._confirm)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok)
        self._ok = buttons.button(QDialogButtonBox.Ok)
        self._ok.setEnabled(False)
        self._confirm.toggled.connect(self._ok.setEnabled)
        buttons.accepted.connect(self.accept)
        layout.addWidget(buttons)

    def _copy(self, phrase: str) -> None:
        clipboard = QApplication.clipboard()
        if clipboard is None:
            return
        # Digest rather than the phrase itself, so the dialog does not hold a
        # second plaintext copy just to know what to clear.
        self._copied_digest = hashlib.sha256(phrase.encode("utf-8")).digest()
        clipboard.setText(phrase)
        self._copy_notice.setText(
            "Copied. Cleared when this dialog closes -- but clipboard "
            "HISTORY tools may still retain it."
        )

    def done(self, result: int) -> None:
        """Clear our own clipboard copy before releasing the dialog.

        The mnemonic is a permanent account secret, so leaving it on the
        clipboard outlives any warning we could print. Cleared only when the
        clipboard still holds what we put there -- comparing digests so we
        never wipe something the operator copied afterwards.

        This does NOT defeat clipboard-history tools, and the notice says so
        rather than implying the secret is gone.
        """
        clipboard = QApplication.clipboard()
        if clipboard is not None and self._copied_digest is not None:
            current = clipboard.text()
            if hashlib.sha256(current.encode("utf-8")).digest() ==                     self._copied_digest:
                clipboard.clear()
            del current
        self._copied_digest = None
        super().done(result)

    @property
    def confirmed(self) -> bool:
        return bool(self._confirm.isChecked())


class _RestoreDialog(QDialog):
    """Take 24 words back in. Validation lives in the service, not here."""

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Restore Permuto identity")
        self.setModal(True)
        self.setMinimumWidth(560)
        layout = QVBoxLayout(self)

        label = QLabel("Enter your 24-word recovery phrase, separated by spaces:")
        label.setWordWrap(True)
        label.setStyleSheet(f"color: {_C.TEXT_PRIMARY};")
        layout.addWidget(label)

        self._edit = QTextEdit()
        self._edit.setFixedHeight(100)
        self._edit.setStyleSheet(
            f"background: {_C.ELEVATED_BG}; color: {_C.TEXT_PRIMARY}; "
            f"border: 1px solid {_C.BORDER};"
        )
        layout.addWidget(self._edit)

        buttons = QDialogButtonBox(
            QDialogButtonBox.Ok | QDialogButtonBox.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    @property
    def phrase(self) -> str:
        return self._edit.toPlainText().strip()


# --------------------------------------------------------------------------- #
# Worker
# --------------------------------------------------------------------------- #

class _RegistrationWorker(QObject):
    """Runs the link/auth flow and the standing lookup off the UI thread."""

    finished = Signal(dict)

    def __init__(self, identity: Any, action: str) -> None:
        super().__init__()
        self._identity = identity
        self._action = action

    @Slot()
    def run(self) -> None:
        from gui.services.permuto import auth

        try:
            if self._action == "register":
                reg = auth.register(self._identity)
                # Verify against the leaderboard rather than trusting the auth
                # response: "registered" should mean "the venue lists us".
                entry = auth.leaderboard_entry(reg.user_id)
                self.finished.emit(
                    {
                        "ok": True,
                        "user_id": reg.user_id,
                        "trading_address": reg.trading_address,
                        "listed": entry is not None,
                        "entry": entry,
                    }
                )
            else:
                info = self._identity.info()
                entry = (
                    auth.leaderboard_entry(info.user_id) if info.user_id else None
                )
                self.finished.emit(
                    {"ok": True, "listed": entry is not None, "entry": entry,
                     "user_id": info.user_id or "",
                     "trading_address": info.trading_address or ""}
                )
        except Exception as exc:  # noqa: BLE001 - surfaced to the operator
            _log.exception("permuto: %s failed", self._action)
            self.finished.emit({"ok": False, "error": str(exc)})


# --------------------------------------------------------------------------- #
# Page
# --------------------------------------------------------------------------- #

def _default_identity_factory():
    """Build a :class:`PermutoIdentity` over the app's real secrets.yaml.

    Resolved lazily and per call, so the page picks up a config-directory
    change without a restart and imports cleanly in a headless test run.
    """
    from pathlib import Path

    from gui.services.permuto.identity import PermutoIdentity
    from gui.services.warp.service import _SecretsFileIO

    try:
        from gui.utils import default_config_path

        base = Path(default_config_path()).resolve().parent
    except Exception:  # noqa: BLE001 - fall back to the working directory
        base = Path.cwd()

    # default_protector() RAISES on every non-Windows platform. Calling it
    # eagerly here meant _create_page_widget caught the exception during
    # construction (refresh() runs the factory) and replaced the whole page
    # with "Permuto (not yet implemented)" on Linux and macOS -- for a
    # project that is open source and used worldwide, that silently removes
    # the feature for most of its users.
    #
    # Reading and displaying PUBLIC identity state needs no protector at
    # all, so it is resolved lazily: PermutoIdentity only asks for one when
    # it actually has to wrap or unwrap the key, and the error surfaces then,
    # attached to the operation that needs it.
    return PermutoIdentity(_SecretsFileIO(base / "secrets.yaml"))


class PermutoWidget(QWidget):
    """Identity + registration surface for the Permuto perps venue."""

    def __init__(
        self,
        identity_factory: Any = None,
        parent: Optional[QWidget] = None,
    ) -> None:
        super().__init__(parent)
        # Default factory so the page can be built by _create_page_widget like
        # every other tab; tests inject a fake instead of touching the real
        # secrets file or requiring DPAPI.
        self._identity_factory = identity_factory or _default_identity_factory
        self._thread: Optional[QThread] = None
        self._worker: Optional[_RegistrationWorker] = None
        self._build()
        self.refresh()

    # -- construction ------------------------------------------------------- #

    def _build(self) -> None:
        layout = QVBoxLayout(self)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(14)

        title = QLabel("Permuto Capital -- volatility perps")
        title.setStyleSheet(
            f"color: {_C.TEXT_PRIMARY}; font-size: 18px; font-weight: bold;"
        )
        layout.addWidget(title)

        subtitle = QLabel(
            "Market-maker prizes require the BLS wallet identity, not OAuth. "
            "The key below is generated and held locally -- no third-party "
            "wallet, no WalletConnect."
        )
        subtitle.setWordWrap(True)
        subtitle.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        layout.addWidget(subtitle)

        line = QFrame()
        line.setFrameShape(QFrame.HLine)
        line.setStyleSheet(f"color: {_C.BORDER};")
        layout.addWidget(line)

        self._identity_lbl = QLabel("")
        self._identity_lbl.setWordWrap(True)
        self._identity_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self._identity_lbl.setStyleSheet(
            f"color: {_C.TEXT_PRIMARY}; font-family: monospace;"
        )
        layout.addWidget(self._identity_lbl)

        self._backup_box = QCheckBox(
            "I have safely stored my 24-word recovery phrase"
        )
        self._backup_box.setStyleSheet(f"color: {_C.TEXT_PRIMARY};")
        self._backup_box.toggled.connect(self._on_backup_toggled)
        layout.addWidget(self._backup_box)

        row = QHBoxLayout()
        self._create_btn = QPushButton("Create identity")
        self._create_btn.clicked.connect(self._on_create)
        row.addWidget(self._create_btn)

        self._restore_btn = QPushButton("Restore from phrase")
        self._restore_btn.clicked.connect(self._on_restore)
        row.addWidget(self._restore_btn)

        self._register_btn = QPushButton("Register with Permuto")
        self._register_btn.clicked.connect(self._on_register)
        row.addWidget(self._register_btn)

        self._check_btn = QPushButton("Check leaderboard")
        self._check_btn.clicked.connect(self._on_check)
        row.addWidget(self._check_btn)

        row.addStretch(1)
        layout.addLayout(row)

        self._status = QLabel("")
        self._status.setWordWrap(True)
        self._status.setTextInteractionFlags(Qt.TextSelectableByMouse)
        layout.addWidget(self._status)

        layout.addStretch(1)

    # -- state -------------------------------------------------------------- #

    def refresh(self) -> None:
        """Re-read the identity and put the page into exactly one state."""
        identity = self._identity_factory()
        try:
            info = identity.info()
        except Exception:  # noqa: BLE001 - absence is a normal state here
            self._identity_lbl.setText("No Permuto identity on this machine.")
            self._backup_box.setChecked(False)
            self._backup_box.setEnabled(False)
            self._create_btn.setEnabled(True)
            self._restore_btn.setEnabled(True)
            self._register_btn.setEnabled(False)
            self._check_btn.setEnabled(False)
            self._set_status("", "")
            return

        self._identity_lbl.setText(
            "BLS public key:  %s\nTrading address: %s"
            % (info.pubkey, info.trading_address or "(resolved on registration)")
        )
        self._backup_box.setEnabled(not info.backup_confirmed)
        self._backup_box.setChecked(info.backup_confirmed)
        self._create_btn.setEnabled(False)
        self._restore_btn.setEnabled(True)
        # Nothing to look up without a user id: searching for "" finds
        # nothing and would render as "Registered -- not on the leaderboard
        # yet", claiming a registration that never happened.
        self._check_btn.setEnabled(bool(info.registered and info.user_id))

        if info.registered:
            self._register_btn.setEnabled(False)
            # Green is reserved for a listing we actually saw. `registered`
            # alone only means the link call returned; rendering that green
            # on every later refresh would quietly promote an unverified
            # account to confirmed, which is exactly what the leaderboard
            # read-back exists to prevent.
            if info.listing_verified:
                self._set_status(
                    "Successfully registered  --  user %s"
                    % (info.user_id or "")[:16],
                    _C.PROFIT_GREEN,
                )
            else:
                self._set_status(
                    "Registered  --  not yet confirmed on the leaderboard. "
                    "Press Check leaderboard to verify.",
                    _C.WARNING_YELLOW,
                )
        else:
            # The gate: no registration until the words are written down.
            self._register_btn.setEnabled(info.backup_confirmed)
            if info.backup_confirmed:
                self._set_status("Ready to register.", _C.TEXT_SECONDARY)
            else:
                self._set_status(
                    "Confirm your recovery phrase is stored before "
                    "registering -- the key cannot be changed afterwards.",
                    _C.WARNING_YELLOW,
                )

    def _set_status(self, text: str, colour: str) -> None:
        self._status.setText(text)
        if colour:
            self._status.setStyleSheet(
                "color: %s; font-size: 14px; font-weight: bold;" % colour
            )

    def stop_background_work(self) -> None:
        """Join the worker before teardown.

        Qt aborts the process (qFatal) if a QThread is destroyed while still
        running, and these requests take up to 30 seconds. MainWindow calls
        this for every page that owns a thread.
        """
        thread = self._thread
        if thread is None:
            return
        # quit() only asks the event loop to stop, and the worker is blocked
        # in a socket read for up to 30 seconds -- so waiting politely can
        # expire with the thread still running, and returning here would let
        # closeEvent() destroy the widget underneath it. That is the exact
        # "QThread: Destroyed while thread is still running" abort this
        # method exists to prevent. Same terminate-and-wait fallback the
        # other page-owned workers use (settings.py, wallet_balances.py).
        thread.quit()
        if not thread.wait(10000):
            _log.warning("permuto: worker thread did not stop; terminating")
            thread.terminate()
            thread.wait(1000)
        # Cleared only AFTER the thread is actually down, so nothing can
        # observe a half-torn-down state.
        self._thread = None
        self._worker = None

    # -- actions ------------------------------------------------------------ #

    @Slot()
    def _on_create(self) -> None:
        identity = self._identity_factory()
        try:
            _pubkey, phrase = identity.create()
        except Exception as exc:  # noqa: BLE001
            QMessageBox.critical(self, "Could not create identity", str(exc))
            return

        dialog = _RecoveryPhraseDialog(phrase, self)
        dialog.exec()
        if dialog.confirmed:
            identity.mark_backup_confirmed()
        # The phrase goes out of scope here and is never written anywhere.
        self.refresh()

    @Slot()
    def _on_restore(self) -> None:
        dialog = _RestoreDialog(self)
        if not dialog.exec():
            return
        identity = self._identity_factory()
        try:
            identity.restore(dialog.phrase)
        except Exception as exc:  # noqa: BLE001
            QMessageBox.critical(self, "Could not restore identity", str(exc))
            return
        self.refresh()

    @Slot(bool)
    def _on_backup_toggled(self, checked: bool) -> None:
        if not checked:
            return
        identity = self._identity_factory()
        try:
            if identity.exists():
                identity.mark_backup_confirmed()
        except Exception:  # noqa: BLE001
            return
        self.refresh()

    @Slot()
    def _on_register(self) -> None:
        confirm = QMessageBox.question(
            self,
            "Register with Permuto?",
            "This links your BLS key to a Permuto account permanently.\n\n"
            "The key cannot be changed later -- on this venue the key IS the "
            "account. Make sure your 24-word phrase is stored somewhere you "
            "will still have in a year.\n\nProceed?",
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No,
        )
        if confirm != QMessageBox.Yes:
            return
        self._run("register", "Registering...")

    @Slot()
    def _on_check(self) -> None:
        self._run("check", "Checking leaderboard...")

    def _run(self, action: str, pending: str) -> None:
        if self._thread is not None:
            return  # already busy
        self._set_status(pending, _C.INFO_BLUE)
        for btn in (self._register_btn, self._check_btn, self._restore_btn):
            btn.setEnabled(False)

        self._thread = QThread(self)
        self._worker = _RegistrationWorker(self._identity_factory(), action)
        self._worker.moveToThread(self._thread)
        self._thread.started.connect(self._worker.run)
        self._worker.finished.connect(self._on_finished)
        self._thread.start()

    @Slot(dict)
    def _on_finished(self, result: dict) -> None:
        thread, self._thread = self._thread, None
        self._worker = None
        if thread is not None:
            thread.quit()
            thread.wait(5000)

        if not result.get("ok"):
            # refresh() first (it restores button state), then set the message,
            # because refresh() overwrites the status line.
            self.refresh()
            self._set_status("Failed: %s" % result.get("error", ""), _C.LOSS_RED)
            return

        if result.get("user_id") and result.get("trading_address"):
            try:
                self._identity_factory().mark_registered(
                    user_id=result["user_id"],
                    trading_address=result["trading_address"],
                    listing_verified=bool(result.get("listed")),
                )
            except Exception as exc:  # noqa: BLE001
                # A logged-and-continue here would show green while nothing
                # was saved: after a restart the identity reads unregistered
                # and the operator was told otherwise.
                _log.exception("permuto: could not persist registration")
                self.refresh()
                self._set_status(
                    "Linked with Permuto, but SAVING it failed: %s  "
                    "Do not re-register -- the account exists. Fix the "
                    "secrets file and press Check leaderboard." % exc,
                    _C.LOSS_RED,
                )
                return

        self.refresh()

        if result.get("listed"):
            entry = result.get("entry") or {}
            self._set_status(
                "Successfully registered  --  listed on the Permuto "
                "leaderboard as %s (%s)"
                % (result["user_id"][:16], entry.get("category", "")),
                _C.PROFIT_GREEN,
            )
        else:
            # Registered but not yet listed is normal: the board is rebuilt
            # periodically. Say that, rather than implying failure.
            self._set_status(
                "Registered  --  not on the leaderboard yet. It is rebuilt "
                "periodically; press Check leaderboard again shortly.",
                _C.WARNING_YELLOW,
            )
