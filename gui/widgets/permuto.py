"""Permuto page: create the trading identity, back it up, register, verify.

Five states, and the page shows exactly one:

1. **No identity** -- offer to create it, or restore from 24 words.
2. **Identity, backup not confirmed** -- refuse to register. The words are the
   only thing standing between the operator and a lost account, and the moment
   after creation is the only moment they can be written down.
3. **Backed up, not registered** -- the Register button is live.
4. **Link unfinished** -- a registration started and its outcome was not
   recorded, either because the venue's answer was lost or because the local
   write failed. Register is shut and the only button offered is the one that
   RECOVERS: the key may already be the account, and a second permanent link
   is the one action with no undo.
5. **Registered** -- green confirmation, with standing read back from the
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
import time
from datetime import datetime
from typing import Any, Optional

from PySide6.QtCore import QObject, Qt, QThread, QTimer, Signal, Slot
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QDialog,
    QDialogButtonBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS as _C
from gui.widgets.sub_tabs import SubTabPages

_log = logging.getLogger(__name__)

PERMUTO_URL = "https://perps.permuto.capital"

#: Lines kept in the Activity section. Bounded because this is a diagnostic
#: view, not a log: an unbounded QPlainTextEdit on a 5s poll grows without
#: limit over a 102-hour contest.
_ACTIVITY_LINES = 500

#: How often the Markets section re-reads the venue.
#:
#: Matches the oracle's own resample interval. Faster would show the same
#: number twice; slower would show a figure the venue has already replaced,
#: on a series measured moving 10-13% in seconds.
_MARKETS_POLL_MS = 5000

#: Section indices, in the order they are added.
_SECTION_IDENTITY = 0
_SECTION_MARKETS = 1
_SECTION_QUOTING = 2
_SECTION_ACTIVITY = 3


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
        grid = self._grid = QTextEdit()
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
        # ALL copying must go through _copy(), because done() can only clear a
        # copy it knows the digest of. setReadOnly leaves
        # TextSelectableByMouse|ByKeyboard and the standard Select All / Copy
        # menu in place, so Ctrl+C on the grid -- the natural gesture when
        # pasting into a password manager -- put the whole mnemonic on the
        # clipboard without entering _copy(), the digest guard short-circuited,
        # and the notice's promise that it is "cleared when this dialog closes"
        # became false. Mouse selection is the worse half on X11 and Wayland:
        # it fills the PRIMARY buffer, which clipboard.clear() does not touch
        # at all. Same closure base_wallet.py applies to its key field.
        grid.setContextMenuPolicy(Qt.ContextMenuPolicy.NoContextMenu)
        grid.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        grid.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
        layout.addWidget(grid)

        copy_row = QHBoxLayout()
        copy_btn = self._copy_btn = QPushButton("Copy to clipboard")
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
            digest = hashlib.sha256(current.encode("utf-8")).digest()
            if digest == self._copied_digest:
                clipboard.clear()
            del current
        self._copied_digest = None
        super().done(result)

    def scrub(self) -> None:
        """Drop every retained copy of the phrase held by this dialog.

        exec() hides a parented dialog, it does not destroy it -- so without
        this the word grid and the copy-button closure keep the mnemonic
        alive as children of the page for the life of the window.
        """
        self._grid.clear()
        try:
            self._copy_btn.clicked.disconnect()
        except (RuntimeError, TypeError):  # already disconnected
            pass
        self._copy_notice.clear()

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

    def scrub(self) -> None:
        """Drop the typed phrase. exec() hides this dialog, it does not
        destroy it, so the editor keeps the secret alive as a child of the
        page until the window closes."""
        self._edit.clear()

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
                result = self._register(auth)
            elif self._action == "reconcile":
                result = self._reconcile(auth)
            else:
                result = self._check(auth)
        except auth.PermutoLinkIndeterminate as exc:
            # NOT an ordinary failure. The venue may already own this key
            # permanently, and the durable marker written before the request
            # is what stops the page offering to link it a second time.
            _log.exception("permuto: the link outcome is unknown")
            self.finished.emit(
                {"ok": False, "indeterminate": True, "error": str(exc)}
            )
        except Exception as exc:  # noqa: BLE001 - surfaced to the operator
            _log.exception("permuto: %s failed", self._action)
            self.finished.emit({"ok": False, "error": str(exc)})
        else:
            self.finished.emit(result)

    def _register(self, auth) -> dict:
        reg = auth.register(self._identity)
        # THE LINK IS NOW PERMANENT AND CANNOT BE REDONE, so the durable
        # write happens HERE, at the side-effect boundary, before anything
        # that can block. It used to happen in _on_finished, on the UI thread,
        # AFTER a paginated leaderboard read whose every page carries a
        # 30-second socket timeout -- and stop_background_work() terminates
        # this thread 10 seconds into a close. Shutting the window shortly
        # after a successful link therefore destroyed the only record of a
        # permanently linked account, leaving a live Register button pointing
        # at a key the venue already owned.
        try:
            self._identity.mark_registered(
                user_id=reg.user_id,
                trading_address=reg.trading_address,
                listing_verified=False,
            )
        except Exception as exc:  # noqa: BLE001
            _log.exception("permuto: linked, but the registration would not save")
            return {
                "ok": False,
                "linked": True,
                "user_id": reg.user_id,
                "trading_address": reg.trading_address,
                "error": str(exc),
            }

        result = {
            "ok": True,
            "user_id": reg.user_id,
            "trading_address": reg.trading_address,
            "listed": False,
            "entry": None,
        }
        self._verify(auth, reg.user_id, result)
        return result

    def _check(self, auth) -> dict:
        info = self._identity.info()
        result = {
            "ok": True,
            "listed": False,
            "entry": None,
            "user_id": info.user_id or "",
            "trading_address": info.trading_address or "",
        }
        self._verify(auth, info.user_id or "", result)
        return result

    def _reconcile(self, auth) -> dict:
        """Ask the venue whether this key is already linked. Links nothing.

        The answer to an indeterminate link. ``reconcile_registration`` reads
        the route documented as having no link side effects, so the question
        cannot become the act.
        """
        found = auth.reconcile_registration(self._identity)
        if found is None:
            # [review] KEEP the marker. This branch used to clear it, on the
            # reasoning that the venue not knowing the key proves the attempt
            # failed -- and that reasoning died when reconcile_registration
            # was corrected: the leaderboard is a POSITIVE-only oracle, so
            # absence means UNCONFIRMED, not unlinked. A linked account that
            # has not traded may simply not be listed yet.
            #
            # Clearing here re-enabled Register for a key the venue may
            # already own, which is the one action with no undo. The
            # operator is not stranded: the marker leaves the page in the
            # unfinished-link state, they can reconcile again later, and if
            # they conclude the account really does not exist they can
            # discard the identity deliberately -- discard_unregistered()
            # still permits that, because a link attempt is not a
            # registration.
            return {
                "ok": True, "reconciled": True, "linked": False,
                "unresolved": True,
            }

        user_id, trading_address = found
        self._identity.mark_registered(
            user_id=user_id, trading_address=trading_address,
            listing_verified=False,
        )
        result = {
            "ok": True,
            "reconciled": True,
            "linked": True,
            "user_id": user_id,
            "trading_address": trading_address,
            "listed": False,
            "entry": None,
        }
        self._verify(auth, user_id, result)
        return result

    def _verify(self, auth, user_id: str, result: dict) -> None:
        """Leaderboard read-back. Never fatal, on ANY branch.

        The register branch always demoted a read-back failure to
        ``verify_error``; the check branch did not, so the identical
        PermutoAuthError from a 503 mid-rebuild came back as ``ok: False`` and
        the red "Failed" wiped a green listing that was already earned and
        persisted -- for an operator who pressed the button the amber messages
        told them to press. One helper now, so the two cannot drift again.
        """
        if not user_id:
            return
        try:
            entry = auth.leaderboard_entry(user_id)
        except Exception as exc:  # noqa: BLE001
            _log.warning("permuto: the leaderboard read-back failed: %s", exc)
            result["verify_error"] = str(exc)
            return
        result["listed"] = entry is not None
        result["entry"] = entry


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


def _default_market_reader() -> dict:
    """Oracle prices and the pause flag, from the public routes.

    Deliberately the ONLY network call this page makes without a session:
    both routes are unauthenticated reads, so opening the Markets section can
    never be an action against the account.
    """
    from gui.services.permuto.auth import _request

    prices = (_request("GET", "/info/oracle") or {}).get("prices") or {}
    flags = (_request("GET", "/info/meta") or {}).get("flags") or {}
    return {"prices": prices, "trading_paused": bool(flags.get("trading_paused"))}


def _scrolled(inner: QWidget) -> QScrollArea:
    """Wrap a section so a narrow window scrolls it instead of clipping it."""
    area = QScrollArea()
    area.setWidgetResizable(True)
    area.setWidget(inner)
    area.setStyleSheet(
        "QScrollArea { border: none; background: transparent; }")
    return area


class _MarketWorker(QObject):
    """One read of the public routes, off the GUI thread.

    [review] This used to run inline in the QTimer callback, and the default
    reader makes two sequential requests with a 30-second timeout each -- so
    a slow venue froze the entire application for up to a minute on every
    poll, on a 5-second timer. The rest of this page already puts network
    work on a worker; the Markets section had quietly not.
    """

    done = Signal(object)     # dict on success
    failed = Signal(str)

    def __init__(self, reader: Any) -> None:
        super().__init__()
        self._reader = reader

    @Slot()
    def run(self) -> None:
        try:
            self.done.emit(self._reader())
        except Exception as exc:  # noqa: BLE001 - reported, never raised
            self.failed.emit(str(exc))


class _CloseWorker(QObject):
    """Read-and-plan, or send, an operator close. Off the GUI thread.

    Two modes rather than one, because the operator must SEE the plan --
    contracts and notional, per market -- before anything is sent. A
    one-shot button that reports what it did afterwards is not a decision,
    it is a surprise.

    In send mode the worker receives the legs that were already approved by
    the operator.  It re-reads the fresh venue position and clamps each leg
    to it before sending, so the actual order can never exceed or differ
    from what was confirmed on screen.
    """

    planned = Signal(object)      # (legs, summary)
    sent = Signal(object)         # result dict
    failed = Signal(str)

    def __init__(self, identity: Any, fraction: float, mode: str,
                 tif: str = "ioc",
                 approved_legs: Optional[list] = None) -> None:
        super().__init__()
        self._identity = identity
        self._fraction = fraction
        self._mode = mode
        self._tif = tif
        self._approved_legs: list = approved_legs or []

    @Slot()
    def run(self) -> None:
        try:
            from gui.services.permuto import close_out
            from gui.services.permuto.client import PermutoClient

            user_id = ""
            try:
                user_id = getattr(self._identity.info(), "user_id", "") or ""
            except Exception:  # noqa: BLE001 - unregistered identity has none
                pass
            client = PermutoClient(self._identity, user_id=user_id)
            client.ensure_session(time.time())
            now = time.time()

            if self._mode == "plan":
                try:
                    prices = _default_market_reader().get("prices") or {}
                except Exception:  # noqa: BLE001 - notional is a nicety
                    prices = {}
                positions = close_out.read_positions(client, now)
                legs = (close_out.plan_close(positions, self._fraction)
                        if positions else [])
                self.planned.emit((legs, close_out.describe(legs, prices)))
                return

            # send mode: use the operator-approved legs, clamped against a
            # fresh read -- not a new plan computed from scratch.
            self.sent.emit(close_out.send_close(
                client, now, self._approved_legs, tif=self._tif))
        except Exception as exc:  # noqa: BLE001 - shown, never raised
            self.failed.emit(str(exc))


class PermutoWidget(QWidget):
    """Identity + registration surface for the Permuto perps venue."""

    def __init__(
        self,
        identity_factory: Any = None,
        parent: Optional[QWidget] = None,
        market_reader: Any = None,
    ) -> None:
        super().__init__(parent)
        # Default factory so the page can be built by _create_page_widget like
        # every other tab; tests inject a fake instead of touching the real
        # secrets file or requiring DPAPI.
        self._identity_factory = identity_factory or _default_identity_factory
        self._thread: Optional[QThread] = None
        self._worker: Optional[_RegistrationWorker] = None
        self._worker_identity: Any = None
        # Identifiers from a link that succeeded and could not be written.
        # Held so refresh() can keep Register shut and offer to save them
        # again; the old handler discarded them and then re-enabled Register
        # under a message telling the operator not to press it.
        self._pending_registration: Optional[dict] = None
        #: True once a reconcile has asked the venue and could not resolve
        #: the link. Gates the discard escape hatch -- see _on_discard.
        self._reconcile_unresolved: bool = False
        # Injectable so tests exercise the Markets section without a socket.
        self._market_reader = market_reader or _default_market_reader
        self._markets_timer: Optional[QTimer] = None
        self._markets_thread: Optional[QThread] = None
        self._close_thread: Optional[QThread] = None
        self._close_worker: Optional[QObject] = None
        self._close_fraction: float = 1.0
        #: True while the quoting loop owns a venue session.
        self._quoting_live: bool = False
        self._markets_worker: Optional[Any] = None
        # [2026-08-31] Target stays SMALL, cap goes wide, and the two are
        # deliberately no longer equal.
        #
        # They were both $1,200, which meant one nearly-complete quote fill
        # took a flat market straight to its position limit -- and at the
        # limit the runner drops to a single reduce-only leg, which earns
        # exactly zero depth (depth_credit_usd skips reduce-only legs). The
        # loop therefore spent ~95% of the contest's first session unable
        # to score at all. A cap has to sit several fills away from the
        # quote size, not one.
        #
        # The cap is sized to TOLERATE the ~$188k position already on the
        # book so the loop can quote two-sided again; it is not an
        # invitation to build one. The target stays at $1,200 on purpose:
        # the 2026-08-31 recovery review is explicit that "quote
        # correctness and uptime dominate size" and that $25k/market on the
        # current code path would only multiply rejected orders. Ramp the
        # target only against a measured leaderboard slope.
        self._target_depth_usd = 1_200.0
        self._max_position_usd = 250_000.0
        self._build()
        self.refresh()

    # -- construction ------------------------------------------------------- #

    def _build(self) -> None:
        root = QVBoxLayout(self)
        root.setContentsMargins(24, 24, 24, 16)
        root.setSpacing(10)

        title = QLabel("Permuto Capital -- volatility perps")
        title.setStyleSheet(
            f"color: {_C.TEXT_PRIMARY}; font-size: 18px; font-weight: bold;"
        )
        root.addWidget(title)

        subtitle = QLabel(
            "Market-maker prizes require the BLS wallet identity, not OAuth. "
            "The key is generated and held locally -- no third-party wallet, "
            "no WalletConnect."
        )
        subtitle.setWordWrap(True)
        subtitle.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        root.addWidget(subtitle)

        line = QFrame()
        line.setFrameShape(QFrame.HLine)
        line.setStyleSheet(f"color: {_C.BORDER};")
        root.addWidget(line)

        # Sections open INSIDE this page rather than as siblings in the
        # sidebar: they share one identity and one venue session, and a
        # sidebar entry per section would imply four independent pages.
        self._sections = SubTabPages()
        for label, builder in (
            ("Identity", self._build_identity_page),
            ("Markets", self._build_markets_page),
            ("Quoting", self._build_quoting_page),
            ("Activity", self._build_activity_page),
        ):
            self._sections.add_page(label, _scrolled(builder()))
        self._sections.currentChanged.connect(self._on_section_changed)
        root.addWidget(self._sections, stretch=1)

        # Status lives OUTSIDE the sections, deliberately. It reports on the
        # identity and on link attempts, which are the states an operator
        # must not miss, and burying it in one section would hide the most
        # important line on the page behind a click.
        self._status = QLabel("")
        self._status.setWordWrap(True)
        self._status.setTextInteractionFlags(Qt.TextSelectableByMouse)
        root.addWidget(self._status)

    # -- sections ----------------------------------------------------------- #

    def _build_identity_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 4, 0, 0)
        layout.setSpacing(14)

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

        # Shown only when a link did not finish cleanly. Deliberately the ONLY
        # affordance offered in that state: every other route out of it either
        # links a second time or leaves the operator with a disabled button
        # and an instruction to press it.
        self._recover_btn = QPushButton("Recover registration")
        self._recover_btn.clicked.connect(self._on_recover)
        self._recover_btn.setVisible(False)
        row.addWidget(self._recover_btn)

        # [review] THE WAY OUT of an unfinished link.
        #
        # restore() sets the attempt marker unconditionally, so restoring a
        # never-registered phrase on a fresh machine opens in the unfinished
        # state: Register shut, Create shut, only Recover offered -- and
        # Recover asks a positive-only oracle that can never answer "no".
        # The status text already told the operator to "discard this identity
        # deliberately" and nothing in the page could do it, so the only
        # escape was hand-editing secrets.yaml. With registration closing
        # Monday 17:00 ET that is an entry-blocking trap, not an
        # inconvenience.
        self._discard_btn = QPushButton("Discard this identity")
        self._discard_btn.clicked.connect(self._on_discard)
        self._discard_btn.setVisible(False)
        row.addWidget(self._discard_btn)

        row.addStretch(1)
        layout.addLayout(row)
        layout.addStretch(1)
        return page

    def _build_markets_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 4, 0, 0)
        layout.setSpacing(12)

        blurb = QLabel(
            "Read-only. These are the venue's own numbers, polled from the "
            "public /info routes -- no key and no orders are involved. The "
            "oracle is a 60-second trailing realized-vol estimate resampled "
            "every 5s, so it moves faster than any equity book."
        )
        blurb.setWordWrap(True)
        blurb.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        layout.addWidget(blurb)

        self._markets_lbl = QLabel("Not polling.")
        self._markets_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self._markets_lbl.setStyleSheet(
            f"color: {_C.TEXT_PRIMARY}; font-family: monospace;"
        )
        layout.addWidget(self._markets_lbl)

        row = QHBoxLayout()
        self._markets_btn = QPushButton("Start polling")
        self._markets_btn.setCheckable(True)
        self._markets_btn.toggled.connect(self._on_markets_toggled)
        row.addWidget(self._markets_btn)
        row.addStretch(1)
        layout.addLayout(row)

        self._markets_note = QLabel("")
        self._markets_note.setWordWrap(True)
        self._markets_note.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        layout.addWidget(self._markets_note)

        layout.addStretch(1)
        return page

    def _build_quoting_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 4, 0, 0)
        layout.setSpacing(12)

        blurb = QLabel(
            "The parameters the quoting loop would run with. Depth credit is "
            "min(bid, ask), so the two sides are always sized equally and "
            "inventory is steered by moving BOTH quotes in price -- shrinking "
            "a side would truncate the minimum and cost eligibility."
        )
        blurb.setWordWrap(True)
        blurb.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        layout.addWidget(blurb)

        self._quoting_lbl = QLabel("")
        self._quoting_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self._quoting_lbl.setStyleSheet(
            f"color: {_C.TEXT_PRIMARY}; font-family: monospace;"
        )
        layout.addWidget(self._quoting_lbl)

        row = QHBoxLayout()
        self._arm_btn = QPushButton("Start quoting")
        self._arm_btn.setEnabled(False)
        row.addWidget(self._arm_btn)
        row.addStretch(1)
        layout.addLayout(row)

        self._arm_note = QLabel("")
        self._arm_note.setWordWrap(True)
        self._arm_note.setStyleSheet(f"color: {_C.WARNING_YELLOW};")
        layout.addWidget(self._arm_note)

        # -- operator close ------------------------------------------------ #
        #
        # The quoting loop sheds inventory as a MAKER only, and risk.py
        # deliberately reserves crossing the spread to close as an operator
        # decision. Until this button there was no way to make that decision:
        # the page offered Create/Restore/Register/Check/Recover/Discard/
        # Start polling/Start quoting and nothing else. A doctrine that
        # reserves a choice for a human, in software that gives the human no
        # control, is not a safeguard -- it is a dead end.
        close_frame = QFrame()
        close_frame.setFrameShape(QFrame.HLine)
        close_frame.setStyleSheet(f"color: {_C.BORDER};")
        layout.addWidget(close_frame)

        close_title = QLabel("Close position")
        close_title.setStyleSheet(
            f"color: {_C.TEXT_PRIMARY}; font-weight: bold;")
        layout.addWidget(close_title)

        close_blurb = QLabel(
            "Buys back a short or sells down a long, crossing the spread. "
            "Every leg is reduce-only, so this can only ever shrink a "
            "position -- never open or flip one. You will see exactly what "
            "it intends to send before anything is placed."
        )
        close_blurb.setWordWrap(True)
        close_blurb.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        layout.addWidget(close_blurb)

        close_row = QHBoxLayout()
        self._close_btns = {}
        for frac, label in ((0.25, "Close 25%"), (0.50, "Close 50%"),
                            (1.00, "Close all")):
            btn = QPushButton(label)
            btn.clicked.connect(
                lambda _checked=False, f=frac: self._on_close_clicked(f))
            close_row.addWidget(btn)
            self._close_btns[frac] = btn
        close_row.addStretch(1)
        layout.addLayout(close_row)

        self._close_note = QLabel("")
        self._close_note.setWordWrap(True)
        self._close_note.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        layout.addWidget(self._close_note)

        layout.addStretch(1)
        return page

    # -- operator close ---------------------------------------------------- #

    def close_in_flight(self) -> bool:
        """True while a close worker owns (or is about to own) a session.

        [review] The guard was one-directional. It stopped a close starting
        during live quoting, but not the INVERSE: starting the runner while
        a close plan or send is already in flight opens the quoting
        client's session anyway, and set_quoting_live(True) then merely
        greys out buttons whose worker is already running. Both clients
        would sit in alternating 401/reauth, which is the failure the guard
        exists to prevent, arriving from the other side.
        """
        return getattr(self, "_close_thread", None) is not None

    def set_quoting_live(self, live: bool) -> None:
        """Told by MainWindow when the quoting loop owns a venue session.

        [review] The close worker builds its OWN PermutoClient and calls
        ensure_session(). PermutoClient documents that concurrent renewals
        install different tokens which invalidate each other, so pressing
        Close while the loop is running can put both into alternating
        401/reauth -- at the exact moment the operator is trying to get
        out of a position, which is the worst possible time for the
        session to be contended.

        Rather than race it, the control says why it is unavailable. Stop
        quoting, close, then start again.
        """
        self._quoting_live = bool(live)
        self._set_close_enabled(not self._quoting_live)
        if self._quoting_live:
            self._close_note.setText(
                "Stop quoting first. The close needs its own venue session, "
                "and two sessions for one identity invalidate each other's "
                "tokens.")
        elif self._close_note.text().startswith("Stop quoting first"):
            self._close_note.setText("")

    def _set_close_enabled(self, on: bool) -> None:
        if on and getattr(self, "_quoting_live", False):
            on = False          # the guard above always wins
        for btn in getattr(self, "_close_btns", {}).values():
            btn.setEnabled(on)

    def _on_close_clicked(self, fraction: float) -> None:
        """Phase one: read the venue and show the plan. Sends nothing."""
        if self._close_thread is not None:
            return
        self._set_close_enabled(False)
        self._close_note.setText("Reading positions from the venue...")
        self._start_close_worker(fraction, "plan")

    def _start_close_worker(self, fraction: float, mode: str,
                            approved_legs: Optional[list] = None) -> None:
        thread = QThread(self)
        worker = _CloseWorker(self._identity_factory(), fraction, mode,
                              approved_legs=approved_legs)
        worker.moveToThread(thread)
        thread.started.connect(worker.run)
        worker.planned.connect(self._on_close_planned)
        worker.sent.connect(self._on_close_sent)
        worker.failed.connect(self._on_close_failed)
        for sig in (worker.planned, worker.sent, worker.failed):
            sig.connect(thread.quit)
        thread.finished.connect(self._on_close_thread_finished)
        self._close_thread = thread
        self._close_worker = worker
        self._close_fraction = fraction
        thread.start()

    def _on_close_thread_finished(self) -> None:
        thread, self._close_thread = self._close_thread, None
        self._close_worker = None
        if thread is not None:
            thread.deleteLater()

    @Slot(object)
    def _on_close_planned(self, payload: Any) -> None:
        legs, summary = payload
        self._set_close_enabled(True)
        if not legs:
            self._close_note.setText(summary)
            self._log_activity("Close: nothing to do -- no open positions.")
            return
        # The operator confirms against the ACTUAL numbers, not a percentage.
        box = QMessageBox(self)
        box.setWindowTitle("Confirm close")
        box.setIcon(QMessageBox.Warning)
        box.setText("Send these reduce-only orders to Permuto?")
        box.setInformativeText(
            "%s\n\nThey cross the spread (IOC), so they are intended to "
            "fill immediately at whatever the book offers." % summary)
        box.setStandardButtons(QMessageBox.Cancel | QMessageBox.Ok)
        box.setDefaultButton(QMessageBox.Cancel)
        if box.exec() != QMessageBox.Ok:
            self._close_note.setText("Cancelled -- nothing was sent.")
            self._log_activity("Close: cancelled by operator.")
            return
        self._set_close_enabled(False)
        self._close_note.setText("Sending...")
        self._log_activity("Close: operator confirmed %d leg(s)." % len(legs))
        self._start_close_worker(self._close_fraction, "send",
                                 approved_legs=legs)

    @Slot(object)
    def _on_close_sent(self, result: Any) -> None:
        self._set_close_enabled(True)
        if not result.get("ok"):
            self._close_note.setText("Failed: %s" % result.get("note", ""))
            self._log_activity("Close FAILED: %s" % result.get("note", ""))
            return
        sent = result.get("sent", 0)
        note = result.get("note") or ("%d leg(s) sent" % sent)
        self._close_note.setText(
            "%s. Check the position on the venue -- a reduce-only IOC can "
            "part-fill, and this button does not retry." % note)
        self._log_activity("Close: %s" % note)

    @Slot(str)
    def _on_close_failed(self, message: str) -> None:
        self._set_close_enabled(True)
        self._close_note.setText("Failed: %s" % message)
        self._log_activity("Close FAILED: %s" % message)

    def _build_activity_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 4, 0, 0)
        layout.setSpacing(10)

        blurb = QLabel(
            "Everything this page has done, newest last. Kept in memory only "
            "-- the engine's own log is the durable record."
        )
        blurb.setWordWrap(True)
        blurb.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        layout.addWidget(blurb)

        self._activity = QPlainTextEdit()
        self._activity.setReadOnly(True)
        self._activity.setMaximumBlockCount(_ACTIVITY_LINES)
        self._activity.setStyleSheet(
            f"color: {_C.TEXT_PRIMARY}; font-family: monospace;"
        )
        layout.addWidget(self._activity, stretch=1)
        return page

    # -- section behaviour -------------------------------------------------- #

    def _on_section_changed(self, index: int) -> None:
        """Stop polling when the operator navigates away from Markets.

        A background poll feeding a widget nobody is looking at is a request
        per 5s against a venue we are also a competitor on, for no benefit.
        """
        if index != _SECTION_MARKETS and self._markets_btn.isChecked():
            self._markets_btn.setChecked(False)
        if index == _SECTION_QUOTING:
            self._refresh_quoting()

    def _log_activity(self, message: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S")
        self._activity.appendPlainText("%s  %s" % (stamp, message))

    # -- markets ------------------------------------------------------------ #

    def _on_markets_toggled(self, on: bool) -> None:
        self._markets_btn.setText("Stop polling" if on else "Start polling")
        if not on:
            if self._markets_timer is not None:
                self._markets_timer.stop()
            self._markets_note.setText("")
            self._log_activity("markets: polling stopped")
            return

        if self._markets_timer is None:
            self._markets_timer = QTimer(self)
            self._markets_timer.timeout.connect(self._poll_markets)
        self._markets_timer.start(_MARKETS_POLL_MS)
        self._log_activity("markets: polling every %.0fs"
                           % (_MARKETS_POLL_MS / 1000.0))
        self._poll_markets()

    def _poll_markets(self) -> None:
        """Kick off one read, on a worker thread.

        Overlapping polls are refused rather than queued: the timer fires
        every 5s and a request may take 30, so queuing would build an
        unbounded backlog of stale reads against a venue we compete on.
        """
        if self._markets_thread is not None:
            return

        thread = QThread(self)
        worker = _MarketWorker(self._market_reader)
        worker.moveToThread(thread)
        thread.started.connect(worker.run)
        worker.done.connect(self._on_markets_result)
        worker.failed.connect(self._on_markets_failed)
        for sig in (worker.done, worker.failed):
            sig.connect(thread.quit)
        thread.finished.connect(self._on_markets_thread_finished)
        self._markets_thread = thread
        self._markets_worker = worker
        thread.start()

    def _on_markets_thread_finished(self) -> None:
        thread, self._markets_thread = self._markets_thread, None
        self._markets_worker = None
        if thread is not None:
            thread.deleteLater()

    def _on_markets_failed(self, message: str) -> None:
        self._markets_lbl.setText("unavailable")
        self._markets_note.setText("Last poll failed: %s" % message)
        self._log_activity("markets: poll failed -- %s" % message)

    def _on_markets_result(self, snapshot: Any) -> None:
        if not isinstance(snapshot, dict):
            self._on_markets_failed(
                "venue returned %s, not an object" % type(snapshot).__name__)
            return

        prices = snapshot.get("prices") or {}
        if not prices:
            self._markets_lbl.setText("venue returned no oracle prices")
            return

        lines = ["%-18s %12s" % ("MARKET", "ORACLE")]
        for name in sorted(prices):
            lines.append("%-18s %12s" % (name, prices[name]))
        self._markets_lbl.setText(chr(10).join(lines))

        paused = snapshot.get("trading_paused")
        if paused:
            self._markets_note.setText(
                "TRADING IS PAUSED at the venue. Quotes are rejected while "
                "this holds, and the Sunday reset happens inside a pause.")
        else:
            self._markets_note.setText("Trading is open.")

    # -- quoting ------------------------------------------------------------ #

    def _refresh_quoting(self) -> None:
        """Show the parameters, and say plainly why quoting is not armed."""
        from gui.services.permuto import risk as _risk

        rows = [
            ("target depth per side", "$%.0f" % self._target_depth_usd),
            ("max position", "$%.0f of notional" % self._max_position_usd),
            ("aggressive ring", "+/-2.00%  (depth credit + purge boundary)"),
            ("legal band", "+/-5.00%  (outside is HTTP 400)"),
            ("stop adding risk at", "%.0f%% margin utilisation"
                % (_risk.MAX_MARGIN_UTILISATION * 100)),
            ("shed risk at", "%.0f%% margin utilisation"
                % (_risk.FLATTEN_MARGIN_UTILISATION * 100)),
            ("carried sizing", "1/%.0f of live size (8x stressed IM)"
                % _risk.CARRIED_IM_MULTIPLIER),
        ]
        width = max(len(k) for k, _ in rows)
        self._quoting_lbl.setText(
            chr(10).join("%-*s  %s" % (width, k, v) for k, v in rows))

        try:
            info = self._identity_factory().info()
            registered = bool(info.registered)
        except Exception:  # noqa: BLE001 - no identity is a normal state
            registered = False

        # The button stays disabled, and the reason is spelled out rather
        # than implied by a grey rectangle. Arming this places REAL orders
        # with real collateral, so it is an explicit decision and not a
        # side effect of opening a tab.
        # The loop is armed from the PERMUTO switch in the toolbar, beside
        # the dexie one, because starting and stopping a venue is the same
        # decision on both and belongs in one place. This section describes
        # what it would do; it does not duplicate the control.
        if not registered:
            self._arm_note.setText(
                "The PERMUTO switch in the toolbar refuses to turn on: this "
                "identity is not registered with the venue. Register on the "
                "Identity section first.")
        else:
            self._arm_note.setText(
                "Armed from the PERMUTO switch in the toolbar. Turning it ON "
                "starts quoting with the parameters above and commits real "
                "collateral; turning it OFF cancels every resting order and "
                "shows STOPPING until that cancel is acknowledged. This is an "
                "unhedgeable position on a 60-second realized-vol oracle -- "
                "watch the first session rather than leaving it.")

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
            self._recover_btn.setVisible(False)
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
            self._pending_registration = None

        # A link that started and did not visibly finish, in either of the two
        # ways it can fail: the venue's answer was lost (durable marker), or
        # the answer arrived and the local write did not (pending identifiers).
        # Both mean this key may ALREADY be the account, so Register stays
        # shut -- refresh() re-enabling it on backup_confirmed alone is what
        # made "Do not re-register" a message printed above a live button.
        unfinished = not info.registered and (
            self._pending_registration is not None or info.link_attempted
        )
        self._recover_btn.setVisible(unfinished)
        self._recover_btn.setEnabled(unfinished)

        # Offered only after the venue has actually been asked and could not
        # resolve it. Throwing a key away is not a first move, and the
        # operator must have consulted the leaderboard before being allowed
        # to. discard_unregistered() refuses outright once `registered` is
        # set, which is the backstop underneath this.
        may_discard = unfinished and self._reconcile_unresolved
        self._discard_btn.setVisible(may_discard)
        self._discard_btn.setEnabled(may_discard)
        self._recover_btn.setText(
            "Save registration" if self._pending_registration
            else "Recover registration"
        )

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
        elif unfinished:
            self._register_btn.setEnabled(False)
            if self._pending_registration:
                self._set_status(
                    "Linked with Permuto, but the registration is NOT saved "
                    "on this machine. Do NOT register again -- the account "
                    "exists. Make secrets.yaml writable, then press Save "
                    "registration.",
                    _C.LOSS_RED,
                )
            else:
                self._set_status(
                    "A registration attempt did not complete, so this key MAY "
                    "already be linked to a Permuto account. Linking again is "
                    "neither possible nor safe. Press Recover registration -- "
                    "it asks the venue and links nothing.",
                    _C.LOSS_RED,
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
        """Join EVERY worker before teardown.

        Qt aborts the process (qFatal) if a QThread is destroyed while still
        running, and these requests take up to 30 seconds. MainWindow calls
        this for every page that owns a thread.

        Both threads, not just the registration one: moving the market poll
        off the GUI thread created a second, and a page that joins one of two
        is a page that still aborts -- just less often, which is worse.
        """
        # Stop the timer first, or a fire during teardown starts a thread we
        # have already decided to join.
        if self._markets_timer is not None:
            self._markets_timer.stop()

        self._join(self._thread, "registration")
        self._thread = None
        self._worker = None

        self._join(self._markets_thread, "market poll")
        self._markets_thread = None
        self._markets_worker = None

        self._join(self._close_thread, "operator close")
        self._close_thread = None
        self._close_worker = None

    @staticmethod
    def _join(thread: Optional[QThread], what: str) -> None:
        """Stop one worker, terminating it if it will not go quietly."""
        if thread is None:
            return
        # quit() only asks the event loop to stop, and the worker may be
        # blocked in a socket read for up to 30 seconds -- so waiting
        # politely can expire with the thread still running, and returning
        # would let closeEvent() destroy the widget underneath it. That is
        # the exact "QThread: Destroyed while thread is still running" abort
        # this exists to prevent. Same terminate-and-wait fallback the other
        # page-owned workers use (settings.py, wallet_balances.py).
        thread.quit()
        if not thread.wait(10000):
            _log.warning("permuto: %s thread did not stop; terminating", what)
            thread.terminate()
            thread.wait(1000)

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
        accepted = bool(dialog.exec())
        # BOTH, and the dialog result is the half that was missing. Ticking
        # the box and then pressing Escape left `confirmed` true, so the
        # identity was marked backed up instead of rolled back -- turning the
        # rollback branch below into dead code in precisely the case it was
        # written for, and leaving an unrecoverable key marked as safe.
        confirmed = accepted and dialog.confirmed
        # exec() only HIDES a parented dialog. Its QTextEdit and the copy
        # lambda keep the full phrase alive as children of this page, so the
        # earlier claim that it "goes out of scope" was wrong. Scrub, then
        # schedule destruction.
        dialog.scrub()
        dialog.deleteLater()

        if confirmed:
            identity.mark_backup_confirmed()
            self.refresh()
            return

        # ROLLED BACK, and this is the whole point of the branch.  Disabling
        # OK does not disable Escape or the title-bar close; either returns
        # Rejected.  The identity was already persisted before the modal
        # opened, so leaving it would strand an account whose ONLY recovery
        # phrase had just been discarded -- with Create disabled because a key
        # exists, and the page checkbox able to mark the unrecoverable key as
        # backed up.  Nothing is registered yet, so the key is worth nothing
        # and discarding it is free; keeping it is what costs.
        try:
            identity.discard_unregistered()
        except Exception as exc:  # noqa: BLE001
            _log.exception("permuto: could not roll back the new identity")
            QMessageBox.critical(
                self, "Identity not rolled back",
                "The recovery phrase was dismissed without confirmation, but "
                "the new key could not be removed: %s  Do NOT register it "
                "-- it has no backup. Remove the 'permuto' section from "
                "secrets.yaml and start again." % exc,
            )
        else:
            QMessageBox.information(
                self, "Identity discarded",
                "The recovery phrase was not confirmed, so the new key was "
                "discarded rather than left without a backup. Press Create "
                "identity to start again.",
            )
        self.refresh()

    @Slot()
    def _on_restore(self) -> None:
        dialog = _RestoreDialog(self)
        accepted = bool(dialog.exec())
        # Capture, then scrub, on BOTH paths. exec() only hides a parented
        # dialog, so its editor would otherwise hold the recovery phrase as a
        # child of this page for the lifetime of the window -- the same
        # retention the creation dialog was already fixed for.
        phrase = dialog.phrase if accepted else ""
        dialog.scrub()
        dialog.deleteLater()
        if not accepted:
            return
        identity = self._identity_factory()
        try:
            identity.restore(phrase)
        except Exception as exc:  # noqa: BLE001
            QMessageBox.critical(self, "Could not restore identity", str(exc))
            return
        finally:
            phrase = ""
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

    @Slot()
    def _on_discard(self) -> None:
        """Throw away an identity the venue could not resolve.

        Guarded three ways, because the destructive case -- discarding a key
        that IS linked -- abandons the account permanently and only the 24
        words can bring it back:

          1. discard_unregistered() refuses outright once `registered` is set.
          2. The button appears only after a reconcile has asked the venue
             and come back unresolved.
          3. The operator types the word, rather than clicking Yes. A
             Yes/No box is answered reflexively; this one cannot be.
        """
        from PySide6.QtWidgets import QInputDialog

        typed, ok = QInputDialog.getText(
            self, "Discard this identity",
            "This deletes the only copy of this key on this machine."
            "\n\n"
            "If the key IS linked at Permuto, the account is abandoned "
            "permanently and ONLY your 24 words can bring it back. Keep "
            "the phrase before continuing."
            "\n\n"
            "Type DISCARD to confirm:")
        if not ok or typed.strip() != "DISCARD":
            self._set_status("Discard cancelled -- nothing was removed.",
                             _C.TEXT_SECONDARY)
            return

        try:
            self._identity_factory().discard_unregistered()
        except Exception as exc:  # noqa: BLE001
            self._set_status("Could not discard: %s" % exc, _C.LOSS_RED)
            return

        self._pending_registration = None
        self._reconcile_unresolved = False
        self.refresh()
        self._set_status(
            "Identity discarded. Create a new one, or restore the phrase of "
            "the account you meant to use.", _C.WARNING_YELLOW)

    def _on_recover(self) -> None:
        """Finish a link that did not finish. Never links anything new.

        Two shapes, one button, because to the operator they are one problem:
        we hold identifiers that would not save (write them again), or we hold
        nothing and do not know whether the venue committed (ask it).
        """
        pending = self._pending_registration
        if not pending:
            self._run(
                "reconcile",
                "Asking Permuto whether this key is already linked...",
            )
            return

        # The WORKER's identity for the same reason _on_finished uses it: this
        # records a PERMANENT link, and the factory would rebuild from a
        # config directory that may have changed since the link was made.
        identity = self._worker_identity or self._identity_factory()
        try:
            identity.mark_registered(
                user_id=pending["user_id"],
                trading_address=pending["trading_address"],
            )
        except Exception as exc:  # noqa: BLE001
            _log.exception("permuto: the registration save failed again")
            self.refresh()
            self._set_status(
                "Still could not save the registration: %s  The account "
                "exists either way -- do NOT register again." % exc,
                _C.LOSS_RED,
            )
            return
        self._pending_registration = None
        self.refresh()
        self._set_status(
            "Registration saved. Press Check leaderboard to confirm the "
            "listing.",
            _C.INFO_BLUE,
        )

    def _run(self, action: str, pending: str) -> None:
        if self._thread is not None:
            return  # already busy
        self._set_status(pending, _C.INFO_BLUE)
        for btn in (self._register_btn, self._check_btn, self._restore_btn,
                    self._recover_btn):
            btn.setEnabled(False)

        self._thread = QThread(self)
        # Hold the exact identity handed to the worker so the result is
        # recorded against the key that actually signed.
        self._worker_identity = self._identity_factory()
        self._worker = _RegistrationWorker(self._worker_identity, action)
        self._worker.moveToThread(self._thread)
        self._thread.started.connect(self._worker.run)
        self._worker.finished.connect(self._on_finished)
        # Clearing the Python attributes does not remove a stopped QThread
        # from this page's QObject children, so repeated leaderboard checks
        # accumulated thread and worker objects until the window closed.
        # Same wiring the other worker-owning pages use.
        self._worker.finished.connect(self._thread.quit)
        self._worker.finished.connect(self._worker.deleteLater)
        self._thread.finished.connect(self._thread.deleteLater)
        self._thread.start()

    @Slot(dict)
    def _on_finished(self, result: dict) -> None:
        thread, self._thread = self._thread, None
        self._worker = None
        if thread is not None:
            thread.quit()
            thread.wait(5000)

        if not result.get("ok"):
            if result.get("linked"):
                # The link is real and the save is not. Hold the identifiers
                # so refresh() can keep Register shut and offer to write them
                # again; discarding them is what left the page telling the
                # operator to press a button it had just disabled.
                self._pending_registration = {
                    "user_id": result.get("user_id", ""),
                    "trading_address": result.get("trading_address", ""),
                }
            # refresh() first (it restores button state), then set the message,
            # because refresh() overwrites the status line.
            self.refresh()
            # "Failed:" is the wrong word for both of these -- the account may
            # well exist, and the operator's next move is recovery, not retry.
            if result.get("linked"):
                self._set_status(
                    "Linked with Permuto, but SAVING the registration "
                    "failed: %s  Do NOT register again -- the account "
                    "exists. Make secrets.yaml writable, then press Save "
                    "registration." % result.get("error", ""),
                    _C.LOSS_RED,
                )
                return
            if result.get("indeterminate"):
                # PermutoLinkIndeterminate carries the whole instruction, and
                # the marker it left on disk is what keeps Register shut
                # across a restart.
                self._set_status(result.get("error", ""), _C.LOSS_RED)
                return
            self._set_status("Failed: %s" % result.get("error", ""), _C.LOSS_RED)
            return

        if result.get("reconciled") and not result.get("linked"):
            self._pending_registration = None
            # The venue has now been asked and could not resolve it, which is
            # what unlocks the discard escape hatch. Set BEFORE refresh(),
            # which is what reads it.
            self._reconcile_unresolved = True
            self.refresh()
            # NOT "you can register again". The leaderboard is a
            # positive-only oracle, so a key it does not list may still be
            # linked -- and registering a second time is the one action with
            # no undo. refresh() keeps Register shut because the attempt
            # marker survives.
            self._set_status(
                "Permuto does not LIST this key, which is not proof it was "
                "never linked -- an account that has not traded may not "
                "appear yet. The attempt is still recorded and Register "
                "stays closed. Reconcile again later; if you are satisfied "
                "no account exists, discard this identity deliberately and "
                "create a new one.",
                _C.WARNING_YELLOW,
            )
            return

        if result.get("user_id") and result.get("trading_address"):
            try:
                # The WORKER's identity, not a freshly resolved one. The
                # factory rebuilds from the config directory, so resolving it
                # again here can attach key A's registration to key B if that
                # directory changed while the request was in flight -- and
                # the thing being recorded is a PERMANENT link.
                (self._worker_identity or self._identity_factory()).mark_registered(
                    user_id=result["user_id"],
                    trading_address=result["trading_address"],
                    listing_verified=bool(result.get("listed")),
                )
            except Exception as exc:  # noqa: BLE001
                # The link itself is already durable -- the worker writes it
                # the moment it becomes permanent. What can still fail here is
                # the PROMOTION of listing_verified, so say that. The old
                # "nothing was saved, do not re-register" alarm would now send
                # an operator hunting a problem they do not have.
                _log.exception(
                    "permuto: could not persist the leaderboard verification"
                )
                self.refresh()
                self._set_status(
                    "Registered and saved, but recording the leaderboard "
                    "confirmation failed: %s  Press Check leaderboard again "
                    "once the secrets file is writable." % exc,
                    _C.WARNING_YELLOW,
                )
                return

        self._pending_registration = None
        self.refresh()

        # 119-5: a lookup that misses while the board rebuilds must not
        # retract a verification we already earned and persisted.
        durable_verified = False
        try:
            durable_verified = self._identity_factory().info().listing_verified
        except Exception:  # noqa: BLE001
            pass

        if result.get("verify_error") and not durable_verified:
            self._set_status(
                "Registered with Permuto and saved. The leaderboard check "
                "could not run (%s) -- press Check leaderboard to confirm."
                % result["verify_error"],
                _C.WARNING_YELLOW,
            )
            return

        if result.get("listed") or durable_verified:
            # `entry` is None whenever the green comes from the DURABLE flag
            # rather than this lookup, so the category is unknown -- printing
            # it unconditionally rendered "as <id> ()".
            entry = result.get("entry") or {}
            category = entry.get("category") or ""
            self._set_status(
                "Successfully registered  --  listed on the Permuto "
                "leaderboard as %s%s"
                % (result["user_id"][:16],
                   " (%s)" % category if category else ""),
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
