"""Base Wallet page widget for XOPTrader GUI.

A small operator surface for the app-controlled **Base-network hot wallet** --
the working intermediary between an exchange (e.g. Coinbase) and the warp
bridge: ETH and USDC come in from the exchange, the bridge spends them,
unwraps land back here, and the surplus goes out to the exchange.

The page offers five operations, all executed by
:class:`gui.services.basewallet.BaseWallet` on the warp worker thread:

* **Create wallet** -- generate the first DPAPI-encrypted key (refused if one
  already exists; rotation is the only way to replace a key).
* **Receive** -- display + copy the wallet's single static address. One key is
  one address on EVM chains; a "fresh receive address" only ever comes from
  key rotation.
* **Back up key** -- reveal the active private key in a guarded, masked modal;
    key bytes arrive over a one-shot signal and never enter a cached snapshot.
* **Send** -- transfer ETH or USDC to an external address (EIP-55 validated,
  worst-case gas reserved, balance-checked -- all re-validated by the
  backend).
* **Rotate key** -- new key, sweep USDC then ETH to it, archive the old blob
  in ``warp.retired_keys`` forever. Refused while any warp job is open.

The tab is fed by the warp snapshot forwarded through
``MainWindow._on_bridge_data`` (``data["warp"]["base_wallet"]`` plus the
``wallet_notice`` / ``wallet_action_error`` companions); it never reads key
material from that snapshot and never performs chain I/O itself. Recovery key
bytes use a separate one-shot signal and are cleared when the modal closes.
Actions are surfaced as one Qt signal (``wallet_action_requested``) that the
main window wires to ``WarpService.wallet_action``.

ISO/IEC 5055  -- all public APIs carry type hints and docstrings.
ISO/IEC 25000 -- degrades gracefully across all snapshot shapes (absent,
                 unconfigured, configured, error).
"""

from __future__ import annotations

import hashlib
import html as _html
import logging
from decimal import Decimal, InvalidOperation
from typing import Any, Final, Optional

from PySide6.QtCore import Qt, Signal, Slot
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS as _C

# ---------------------------------------------------------------------------
# Palette aliases
# ---------------------------------------------------------------------------
PRIMARY_GREEN: Final[str] = _C.PRIMARY_GREEN
LIGHT_GREEN: Final[str] = _C.LIGHT_GREEN
DARK_BG: Final[str] = _C.DARK_BG
PANEL_BG: Final[str] = _C.PANEL_BG
ELEVATED_BG: Final[str] = _C.ELEVATED_BG
BORDER: Final[str] = _C.BORDER
TEXT_PRIMARY: Final[str] = _C.TEXT_PRIMARY
TEXT_SECONDARY: Final[str] = _C.TEXT_SECONDARY
TEXT_DISABLED: Final[str] = _C.TEXT_DISABLED
WARNING_YELLOW: Final[str] = _C.WARNING_YELLOW
INFO_BLUE: Final[str] = _C.INFO_BLUE
PROFIT_GREEN: Final[str] = _C.PROFIT_GREEN
LOSS_RED: Final[str] = _C.LOSS_RED

# ---------------------------------------------------------------------------
# Unit conventions (see the operational-map memory)
# ---------------------------------------------------------------------------
_USDC_MICROS: Final[int] = 1_000_000          # USDC has 6 decimals
_WEI_PER_ETH: Final[int] = 10 ** 18           # ETH has 18 decimals
#: Fallback gas-warning floor, used only until a snapshot arrives (or when
#: warp is disabled and publishes no config). The live threshold comes from
#: warp.min_base_eth via the snapshot -- a hardcoded constant could not be
#: raised for a wallet that relays more often.
_LOW_GAS_WEI_FALLBACK: Final[int] = 5_000_000_000_000_000  # 0.005 ETH

_log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Small label / layout helpers (same idiom as gui.widgets.warp)
# ---------------------------------------------------------------------------

def _section_label(text: str) -> QLabel:
    """Return a bold section-header label."""
    lbl = QLabel(text)
    font = lbl.font()
    font.setPointSize(11)
    font.setBold(True)
    lbl.setFont(font)
    lbl.setStyleSheet(f"color: {LIGHT_GREEN};")
    return lbl


def _body_label(text: str) -> QLabel:
    """Return a body-text label with word-wrap enabled."""
    lbl = QLabel(text)
    lbl.setWordWrap(True)
    lbl.setStyleSheet(f"color: {TEXT_SECONDARY};")
    return lbl


def _separator() -> QFrame:
    """Horizontal rule separator."""
    line = QFrame()
    line.setFrameShape(QFrame.Shape.HLine)
    line.setFrameShadow(QFrame.Shadow.Sunken)
    line.setStyleSheet(f"color: {BORDER};")
    return line


def _mono_field(placeholder: str, *, read_only: bool = True) -> QLineEdit:
    """Return a monospace field for addresses / amounts."""
    field = QLineEdit()
    field.setReadOnly(read_only)
    field.setPlaceholderText(placeholder)
    field.setStyleSheet(
        f"""
        QLineEdit {{
            background-color: {ELEVATED_BG};
            color: {TEXT_PRIMARY};
            border: 1px solid {BORDER};
            border-radius: 3px;
            padding: 6px 8px;
            font-family: Consolas, 'Courier New', monospace;
            font-size: 11px;
        }}
        QLineEdit:focus {{ border-color: {PRIMARY_GREEN}; }}
        """
    )
    return field


def _usdc(micros: Any) -> str:
    """Format USDC micro-units as a human amount, or an em dash when unknown."""
    try:
        return f"{int(micros) / _USDC_MICROS:,.2f}"
    except (TypeError, ValueError):
        return "—"


def _eth(wei: Any) -> str:
    """Format wei as ETH to 6 dp, or an em dash when unknown."""
    try:
        return f"{int(wei) / _WEI_PER_ETH:.6f}"
    except (TypeError, ValueError):
        return "—"


def _millieth(units: Any) -> str:
    """Format 3-decimal milliETH units, or an em dash when unknown."""
    try:
        return f"{int(units) / 1000:,.3f}"
    except (TypeError, ValueError):
        return "\u2014"


def qr_payload(address: str) -> str:
    """The string a receive-QR encodes: the plain checksummed address.

    Deliberately NOT an ethereum: URI -- exchange scanners (the Coinbase
    app's withdraw flow included) all accept a bare address, while URI
    schemes are inconsistently parsed and some embed a chain id that
    scanners reject.  The network warning lives in the dialog text, where
    a human actually reads it.
    """
    return str(address or "").strip()


def qr_png(address: str, *, scale: int = 8) -> bytes:
    """PNG bytes of the address QR (segno: pure-Python, no imaging stack).

    Error level M and a quiet-zone border per the QR spec defaults --
    phone cameras at desk distance read this reliably at scale 8
    (~330x330 px for an EVM address).
    """
    import io

    import segno

    buf = io.BytesIO()
    segno.make(qr_payload(address), error="m").save(
        buf, kind="png", scale=scale, border=2
    )
    return buf.getvalue()


def _short(value: Any, head: int = 10, tail: int = 6) -> str:
    """Middle-truncate a long hex/address string for compact display."""
    s = str(value or "")
    if len(s) <= head + tail + 1:
        return s
    return f"{s[:head]}…{s[-tail:]}"


def _exact_amount(units: int, decimals: int) -> str:
    """Full-precision human string for base units, no rounding.

    The balance formatters (_usdc: 2dp, _eth: 6dp) are for display; a
    confirmation dialog for an irreversible transfer must show EXACTLY what
    will be sent, so 1.234567 USDC never reads back as '1.23'. Trailing
    zeros are stripped for readability (normalize), and format "f" always
    expands to fixed-point -- never scientific notation.
    """
    return format(Decimal(int(units)).scaleb(-decimals).normalize(), "f")


def parse_asset_amount(text: str, *, decimals: int) -> Optional[int]:
    """Parse a human amount into base units, or ``None`` when invalid.

    Decimal, not float, for the same reasons as the unwrap field: float
    rounds sub-precision inputs into an amount the operator never typed,
    drifts on values like 5.0005, and 1e309 overflows with an exception a
    ValueError-only catch misses. Anything that is not a positive whole
    multiple of the asset's smallest unit is rejected, never rounded.
    """
    try:
        amount = Decimal(str(text).strip())
        if not amount.is_finite() or amount <= 0:
            return None
        # A magnitude bound: Decimal accepts 1e309 happily, and while the
        # backend balance check would refuse it anyway, an absurd amount
        # should never even reach the confirmation dialog. The bound also
        # keeps the modulo below the Decimal context precision, so a 29+
        # significant-digit input cannot raise DivisionImpossible.
        if amount > Decimal(10) ** 12:
            return None
        quantum = Decimal(1).scaleb(-decimals)
        if amount % quantum != 0:
            return None
        return int(amount.scaleb(decimals))
    except InvalidOperation:
        # Any Decimal failure -- unparseable text, or a modulo that would need
        # more digits than the context precision -- is an invalid amount, not
        # a crash in the click handler.
        return None


class _KeyBackupDialog(QDialog):
    """Short-lived, masked view of one Base wallet recovery key."""

    def __init__(
        self,
        address: str,
        recovery: bytearray,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        if len(recovery) != 32:
            raise ValueError(f"recovery key must be 32 bytes, got {len(recovery)}")

        self.setWindowTitle("Back Up Base Wallet Key")
        self.setModal(True)
        self.setMinimumWidth(680)
        self._recovery = recovery
        self._copied_digest: bytes | None = None

        layout = QVBoxLayout(self)
        layout.setContentsMargins(22, 22, 22, 22)
        layout.setSpacing(12)

        title = QLabel("Base wallet recovery key")
        title_font = title.font()
        title_font.setPointSize(13)
        title_font.setBold(True)
        title.setFont(title_font)
        title.setStyleSheet(f"color: {TEXT_PRIMARY};")
        layout.addWidget(title)

        warning = QLabel(
            "<b>Anyone with this key controls this wallet.</b> Store it in an "
            "offline backup or trusted password manager. Never send it in "
            "chat, email, or a support request. Clipboard history and third-party "
            "clipboard managers may retain copied keys; disable them first or "
            "use Show without copying."
        )
        warning.setTextFormat(Qt.TextFormat.RichText)
        warning.setWordWrap(True)
        warning.setStyleSheet(
            f"color: {WARNING_YELLOW}; background: {PANEL_BG}; "
            f"border: 1px solid {WARNING_YELLOW}; border-radius: 4px; "
            "padding: 10px;"
        )
        layout.addWidget(warning)

        layout.addWidget(QLabel("Wallet address"))
        address_field = _mono_field("")
        address_field.setText(str(address))
        layout.addWidget(address_field)

        layout.addWidget(QLabel("Private recovery key"))
        key_row = QHBoxLayout()
        self._key_field = _mono_field("Hidden until Show is clicked")
        self._key_field.setEchoMode(QLineEdit.EchoMode.Password)
        # All copying goes through _copy_key so the current clipboard can be
        # cleared on close. Prevent the read-only field's context menu and
        # keyboard selection from creating an untracked copy.
        self._key_field.setContextMenuPolicy(
            Qt.ContextMenuPolicy.NoContextMenu
        )
        self._key_field.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        key_row.addWidget(self._key_field, 1)

        self._show_btn = QPushButton("Show")
        self._show_btn.setFixedWidth(70)
        self._show_btn.clicked.connect(self._toggle_visibility)
        key_row.addWidget(self._show_btn)

        self._copy_key_btn = QPushButton("Copy key")
        self._copy_key_btn.setFixedWidth(90)
        self._copy_key_btn.clicked.connect(self._copy_key)
        key_row.addWidget(self._copy_key_btn)
        layout.addLayout(key_row)

        self._copy_notice = QLabel(
            "Recovery key copied. If unchanged, the current clipboard is cleared "
            "when this dialog closes; clipboard history may retain it."
        )
        self._copy_notice.setWordWrap(True)
        self._copy_notice.setStyleSheet(f"color: {INFO_BLUE};")
        self._copy_notice.setVisible(False)
        layout.addWidget(self._copy_notice)

        self._saved_check = QCheckBox(
            "I saved this recovery key securely."
        )
        layout.addWidget(self._saved_check)

        buttons = QDialogButtonBox()
        self._confirm_btn = buttons.addButton(
            "I saved it securely", QDialogButtonBox.ButtonRole.AcceptRole
        )
        buttons.addButton("Close", QDialogButtonBox.ButtonRole.RejectRole)
        self._confirm_btn.setEnabled(False)
        self._saved_check.toggled.connect(self._confirm_btn.setEnabled)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _toggle_visibility(self) -> None:
        visible = self._key_field.echoMode() == QLineEdit.EchoMode.Password
        if visible:
            self._key_field.setText("0x" + self._recovery.hex())
            self._key_field.setEchoMode(QLineEdit.EchoMode.Normal)
        else:
            self._key_field.setEchoMode(QLineEdit.EchoMode.Password)
            self._key_field.clear()
        self._show_btn.setText("Hide" if visible else "Show")

    def _copy_key(self) -> None:
        clipboard = QApplication.clipboard()
        if clipboard is not None:
            text = "0x" + self._recovery.hex()
            self._copied_digest = hashlib.sha256(text.encode("utf-8")).digest()
            clipboard.setText(text)
            del text
            self._copy_notice.setVisible(True)

    def done(self, result: int) -> None:
        """Clear temporary UI and clipboard copies before releasing the dialog."""
        clipboard = QApplication.clipboard()
        if clipboard is not None and self._copied_digest is not None:
            current = clipboard.text()
            if hashlib.sha256(current.encode("utf-8")).digest() == self._copied_digest:
                clipboard.clear()
            del current
        self._copied_digest = None
        self._key_field.setEchoMode(QLineEdit.EchoMode.Password)
        self._key_field.clear()
        self._recovery[:] = b"\x00" * len(self._recovery)
        super().done(result)


class BaseWalletWidget(QWidget):
    """Operator page for the Base hot wallet (create / receive / send / rotate).

    Fed from ``data["warp"]``:

    * ``base_wallet``: {configured, address, eth_wei, usdc_micros,
      retired_count, backup_confirmed, error}
    * ``wallet_notice``: last successful action's message (persists until the
      next action);
    * ``wallet_action_error``: the immediately-preceding failure, if any.

    Parameters
    ----------
    parent : QWidget | None
        Parent widget.
    """

    #: (action, payload) -> ``WarpService.wallet_action``. Actions include
    #: create, reveal/confirm backup, sends, and rotation.
    wallet_action_requested = Signal(str, dict)
    #: Informational; the widget also copies to the clipboard itself.
    address_copy_requested = Signal(str)

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._snap: dict[str, Any] = {}
        self._address: str = ""
        # One in-flight action at a time. The gate is keyed off the worker's
        # monotonic wallet_action_seq, NOT "any next snapshot": the ~5s master
        # tick delivers cached snapshots that must NOT re-enable the buttons
        # while a click is still queued on the worker thread, or an impatient
        # second click broadcasts a duplicate irreversible transfer.
        self._action_pending: bool = False
        self._last_seen_seq: int = 0
        self._pending_since_seq: int = 0
        self._backup_dialog_open: bool = False
        self._build_ui()
        self._render()

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def update_data(self, data: dict[str, Any], **_kwargs: Any) -> None:
        """Adopt an aggregated bridge snapshot and refresh the view.

        Parameters
        ----------
        data : dict
            The aggregated snapshot from ``EngineBridge.get_all_data()``. Only
            ``data["warp"]`` is consumed; null-safe when missing.
        """
        self._snap = dict((data or {}).get("warp") or {})
        seq = int(self._snap.get("wallet_action_seq") or 0)
        # Clear the post-click gate only when the worker has PROCESSED an
        # action since we sent ours (seq advanced). Timer ticks carry an
        # unchanged seq and leave the gate closed.
        if self._action_pending and seq > self._pending_since_seq:
            self._action_pending = False
        self._last_seen_seq = seq
        if self.isVisible():
            self._render()

    def showEvent(self, event: Any) -> None:  # noqa: N802 -- Qt override
        """Repaint from the latest snapshot when the tab becomes visible."""
        super().showEvent(event)
        self._render()

    @Slot(str, object, int)
    def present_key_backup(
        self, address: str, recovery: object, action_seq: int
    ) -> None:
        """Display one recovery key, then scrub it and optionally confirm."""
        seq = int(action_seq)
        self._last_seen_seq = max(self._last_seen_seq, seq)
        if self._action_pending and seq > self._pending_since_seq:
            self._action_pending = False
        self._render()

        if not isinstance(recovery, bytearray):
            QMessageBox.critical(
                self, "Key backup unavailable", "The recovery key payload is invalid."
            )
            return
        raw = recovery
        if self._backup_dialog_open:
            raw[:] = b"\x00" * len(raw)
            return

        accepted = False
        self._backup_dialog_open = True
        try:
            dialog = _KeyBackupDialog(str(address), raw, self)
            accepted = dialog.exec() == QDialog.DialogCode.Accepted
        except ValueError as exc:
            QMessageBox.critical(self, "Key backup unavailable", str(exc))
        finally:
            raw[:] = b"\x00" * len(raw)
            self._backup_dialog_open = False

        if accepted:
            self._emit_action("confirm_backup", {})

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------

    def _build_ui(self) -> None:
        """Assemble the page layout."""
        scroll = QScrollArea(self)
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

        container = QWidget()
        container.setStyleSheet(f"background: {DARK_BG};")
        scroll.setWidget(container)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(scroll)

        layout = QVBoxLayout(container)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.setSpacing(16)

        # -- Page title -------------------------------------------------
        title = QLabel("\U0001F537  Base Wallet")
        title_font = title.font()
        title_font.setPointSize(16)
        title_font.setBold(True)
        title.setFont(title_font)
        title.setStyleSheet(f"color: {TEXT_PRIMARY};")
        layout.addWidget(title)

        subtitle = _body_label(
            "The app-controlled hot wallet on the <b>Base network</b> — the "
            "working intermediary between an exchange (e.g. Coinbase) and the "
            "warp bridge. Deposit ETH (gas) and USDC here from the exchange; "
            "bridged and unwrapped funds land here; send the surplus back out. "
            "The key is generated locally and stored DPAPI-encrypted in "
            "secrets.yaml — it never leaves this machine."
        )
        subtitle.setTextFormat(Qt.TextFormat.RichText)
        layout.addWidget(subtitle)

        # -- Status banner ---------------------------------------------
        self._banner = QLabel()
        self._banner.setWordWrap(True)
        self._banner.setTextFormat(Qt.TextFormat.RichText)
        self._banner.setMinimumHeight(40)
        layout.addWidget(self._banner)

        # -- Last-action notice ----------------------------------------
        self._notice = QLabel()
        self._notice.setWordWrap(True)
        # PlainText, explicitly: the notice carries untrusted exception text
        # (wallet_action_error), and the default AutoText format would let a
        # '<...>' inside an RPC error be interpreted as RichText markup.
        self._notice.setTextFormat(Qt.TextFormat.PlainText)
        self._notice.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        self._notice.setVisible(False)
        layout.addWidget(self._notice)

        layout.addWidget(_separator())

        # -- Create (only when no wallet exists) ------------------------
        self._create_section = QWidget()
        create_layout = QVBoxLayout(self._create_section)
        create_layout.setContentsMargins(0, 0, 0, 0)
        create_layout.setSpacing(8)
        create_layout.addWidget(_section_label("Create Wallet"))
        create_layout.addWidget(
            _body_label(
                "No Base wallet exists yet. Creating one generates a fresh key "
                "on this machine and stores it encrypted in secrets.yaml. "
                "Afterwards, use Back up key below before depositing funds. "
                "DPAPI encryption is bound to this Windows user, so a machine "
                "failure without that recovery key destroys access to the funds."
            )
        )
        self._create_btn = self._primary_button("\U0001F195  Create wallet")
        self._create_btn.clicked.connect(self._on_create_clicked)
        create_row = QHBoxLayout()
        create_row.addWidget(self._create_btn)
        create_row.addStretch(1)
        create_layout.addLayout(create_row)
        create_layout.addWidget(_separator())
        layout.addWidget(self._create_section)

        # -- Receive ----------------------------------------------------
        layout.addWidget(_section_label("Receive (deposit address)"))
        layout.addWidget(
            _body_label(
                "This wallet has a single, permanent address — on EVM chains "
                "one key is one address, so there is no \"generate new receive "
                "address\"; a fresh address only ever comes from a key "
                "rotation below. Send <b>ETH or USDC on the Base network "
                "only</b>. Assets sent on any other chain are lost."
            )
        )
        addr_row = QHBoxLayout()
        self._addr_field = _mono_field("No wallet yet — create one above")
        addr_row.addWidget(self._addr_field, 1)
        self._copy_btn = self._small_button("Copy")
        self._copy_btn.setToolTip("Copy the Base address to the clipboard")
        self._copy_btn.clicked.connect(self._on_copy_address)
        addr_row.addWidget(self._copy_btn)
        self._qr_btn = self._small_button("QR")
        self._qr_btn.setToolTip(
            "Show this address as a QR code -- scan it from an exchange "
            "app's withdraw screen (Base network only)"
        )
        self._qr_btn.clicked.connect(self._on_qr_clicked)
        addr_row.addWidget(self._qr_btn)
        layout.addLayout(addr_row)

        bal_row = QHBoxLayout()
        bal_row.setSpacing(24)
        self._usdc_lbl = QLabel()
        self._usdc_lbl.setStyleSheet(f"color: {TEXT_PRIMARY};")
        self._eth_lbl = QLabel()
        self._eth_lbl.setStyleSheet(f"color: {TEXT_PRIMARY};")
        self._millieth_lbl = QLabel()
        self._millieth_lbl.setStyleSheet(f"color: {TEXT_PRIMARY};")
        self._retired_lbl = QLabel()
        self._retired_lbl.setStyleSheet(f"color: {TEXT_SECONDARY};")
        bal_row.addWidget(self._usdc_lbl)
        bal_row.addWidget(self._eth_lbl)
        bal_row.addWidget(self._millieth_lbl)
        bal_row.addWidget(self._retired_lbl)
        bal_row.addStretch(1)
        layout.addLayout(bal_row)

        self._gas_lbl = QLabel()
        self._gas_lbl.setWordWrap(True)
        self._gas_lbl.setStyleSheet(f"color: {WARNING_YELLOW};")
        self._gas_lbl.setVisible(False)
        layout.addWidget(self._gas_lbl)

        # -- Backup nag -------------------------------------------------
        self._backup_row = QWidget()
        backup_layout = QHBoxLayout(self._backup_row)
        backup_layout.setContentsMargins(0, 0, 0, 0)
        self._backup_lbl = QLabel()
        self._backup_lbl.setWordWrap(True)
        backup_layout.addWidget(self._backup_lbl, 1)
        self._backup_btn = self._small_button("Back up key")
        self._backup_btn.setFixedWidth(120)
        self._backup_btn.clicked.connect(self._on_backup_clicked)
        backup_layout.addWidget(self._backup_btn)
        layout.addWidget(self._backup_row)

        layout.addWidget(_separator())

        # -- Send -------------------------------------------------------
        layout.addWidget(_section_label("Send"))
        layout.addWidget(
            _body_label(
                "Transfer to an external Base address — e.g. your Coinbase "
                "deposit address. The amount plus worst-case gas must fit the "
                "balance; the destination's EIP-55 checksum is verified."
            )
        )
        send_row = QHBoxLayout()
        self._send_amount = _mono_field("Amount", read_only=False)
        self._send_amount.setMaximumWidth(160)
        send_row.addWidget(self._send_amount)
        self._send_asset = QComboBox()
        self._send_asset.addItems(["USDC", "ETH"])
        self._send_asset.setStyleSheet(
            f"""
            QComboBox {{
                background-color: {ELEVATED_BG};
                color: {TEXT_PRIMARY};
                border: 1px solid {BORDER};
                border-radius: 3px;
                padding: 6px 8px;
            }}
            """
        )
        send_row.addWidget(self._send_asset)
        self._send_dest = _mono_field(
            "Destination (0x…, EIP-55)", read_only=False
        )
        send_row.addWidget(self._send_dest, 1)
        self._send_btn = self._primary_button("➤  Send")
        self._send_btn.clicked.connect(self._on_send_clicked)
        send_row.addWidget(self._send_btn)
        layout.addLayout(send_row)

        layout.addWidget(_separator())

        # -- Convert (ETH <-> milliETH) -----------------------------------
        layout.addWidget(_section_label("Convert"))
        layout.addWidget(
            _body_label(
                "Wrap ETH into milliETH (warp.green's 1000-per-ETH bridge "
                "asset, no fee) or unwrap milliETH back to ETH. Wrapped "
                "milliETH is what bridges to Chia as wmilliETH.b; unwrapping "
                "refuels gas. ETH amounts are floored to 0.000001 ETH (the "
                "contract's granularity), and wrapping always leaves the "
                "relay-gas minimum untouched."
            )
        )
        convert_row = QHBoxLayout()
        self._convert_amount = _mono_field("Amount", read_only=False)
        self._convert_amount.setMaximumWidth(160)
        convert_row.addWidget(self._convert_amount)
        self._convert_dir = QComboBox()
        self._convert_dir.addItems(
            ["ETH \u2192 milliETH", "milliETH \u2192 ETH"]
        )
        self._convert_dir.setStyleSheet(self._send_asset.styleSheet())
        convert_row.addWidget(self._convert_dir)
        self._convert_btn = self._primary_button("\u21c4  Convert")
        self._convert_btn.clicked.connect(self._on_convert_clicked)
        convert_row.addWidget(self._convert_btn)
        convert_row.addStretch(1)
        layout.addLayout(convert_row)

        layout.addWidget(_separator())

        # -- Rotate -----------------------------------------------------
        layout.addWidget(_section_label("Rotate Key"))
        layout.addWidget(
            _body_label(
                "Generates a fresh key, sweeps the full USDC balance and then "
                "the ETH balance (minus gas) to it, and archives the old key "
                "in secrets.yaml — archived, never deleted, so anything that "
                "later lands at the old address stays recoverable. Use this "
                "as periodic hot-wallet hygiene or after any suspected "
                "exposure. <b>The receive address changes</b>: update any "
                "exchange withdrawal allowlists, and know that deposits "
                "already in flight to the old address will land at the "
                "archived key (recoverable via the runbook). Refused while a "
                "warp bridge job is open."
            )
        )
        rotate_row = QHBoxLayout()
        self._rotate_btn = self._primary_button("\U0001F504  Rotate key")
        self._rotate_btn.clicked.connect(self._on_rotate_clicked)
        rotate_row.addWidget(self._rotate_btn)
        rotate_row.addStretch(1)
        layout.addLayout(rotate_row)

        layout.addStretch(1)

    # ------------------------------------------------------------------
    # Rendering
    # ------------------------------------------------------------------

    def _render(self) -> None:
        """Repaint every panel from ``self._snap`` (handles all shapes)."""
        snap = self._snap
        bw = dict(snap.get("base_wallet") or {})
        configured = bool(bw.get("configured"))
        error = bw.get("error")

        self._render_banner(snap, bw, configured=configured, error=error)
        self._render_notice(snap)

        self._create_section.setVisible(bool(snap) and not configured)
        self._create_btn.setEnabled(not self._action_pending)

        address = str(bw.get("address") or "")
        self._address = address
        self._addr_field.setText(address)
        self._addr_field.setCursorPosition(0)
        self._copy_btn.setEnabled(bool(address))

        if configured and not error:
            self._usdc_lbl.setText(f"USDC: <b>{_usdc(bw.get('usdc_micros'))}</b>")
            self._usdc_lbl.setTextFormat(Qt.TextFormat.RichText)
            self._eth_lbl.setText(f"ETH (gas): <b>{_eth(bw.get('eth_wei'))}</b>")
            self._eth_lbl.setTextFormat(Qt.TextFormat.RichText)
            self._millieth_lbl.setText(
                f"milliETH: <b>{_millieth(bw.get('millieth_units'))}</b>"
            )
            self._millieth_lbl.setTextFormat(Qt.TextFormat.RichText)
        else:
            self._usdc_lbl.setText("USDC: —")
            self._eth_lbl.setText("ETH (gas): —")
            self._millieth_lbl.setText("milliETH: —")
        retired = bw.get("retired_count") or 0
        self._retired_lbl.setText(
            f"Retired keys: {retired}" if retired else ""
        )

        wei = bw.get("eth_wei")
        floor = self._gas_floor_wei()
        low = configured and isinstance(wei, int) and floor > 0 and wei < floor
        if low:
            self._gas_lbl.setText(
                f"⚠️  Low gas: {_eth(wei)} ETH is below the configured reserve "
                f"of {_eth(floor)} ETH. Sends, rotation and bridge relays all "
                "need Base ETH — top the wallet up "
                "(Settings → Fees & Reserves)."
            )
        self._gas_lbl.setVisible(bool(low))

        backup_confirmed = bool(bw.get("backup_confirmed"))
        self._backup_row.setVisible(configured)
        self._backup_btn.setEnabled(configured and not self._action_pending)
        if backup_confirmed:
            self._backup_lbl.setText(
                "Key backup confirmed. Keep the recovery key offline and "
                "separate from this Windows account."
            )
            self._backup_lbl.setStyleSheet(f"color: {PROFIT_GREEN};")
            self._backup_btn.setText("View key")
            self._backup_btn.setToolTip("Reveal the recovery key in a guarded dialog")
        else:
            self._backup_lbl.setText(
                "Key backup not confirmed. Back it up before funding this wallet; "
                "DPAPI recovery is bound to this Windows account."
            )
            self._backup_lbl.setStyleSheet(f"color: {WARNING_YELLOW};")
            self._backup_btn.setText("Back up key")
            self._backup_btn.setToolTip("Reveal and securely back up the recovery key")

        can_act = configured and not self._action_pending
        self._send_btn.setEnabled(can_act)
        self._convert_btn.setEnabled(can_act)
        self._rotate_btn.setEnabled(can_act)
        if not configured:
            tip = "Create a wallet first."
            self._send_btn.setToolTip(tip)
            self._rotate_btn.setToolTip(tip)
        else:
            self._send_btn.setToolTip("Review and broadcast the transfer.")
            self._rotate_btn.setToolTip(
                "Sweep everything to a fresh key and archive the old one."
            )

    def _gas_floor_wei(self) -> int:
        """The operator's configured Base gas reserve, in wei.

        Falls back to a built-in default only while no snapshot has arrived;
        an explicit 0 in config legitimately disables the warning."""
        raw = self._snap.get("min_base_eth_wei")
        try:
            return int(raw)
        except (TypeError, ValueError):
            return _LOW_GAS_WEI_FALLBACK

    def _render_banner(
        self, snap: dict, bw: dict, *, configured: bool, error: Any
    ) -> None:
        if not snap:
            colour, text = TEXT_SECONDARY, "Connecting to the wallet service…"
        elif error:
            colour = WARNING_YELLOW
            # Untrusted exception text into a RichText label: escape it.
            text = f"Wallet <b>unavailable</b>: {_html.escape(str(error))}"
        elif not configured:
            colour = TEXT_SECONDARY
            text = (
                "No Base wallet configured. Create one below to get a deposit "
                "address for ETH and USDC on Base."
            )
        elif not bw.get("backup_confirmed"):
            colour = WARNING_YELLOW
            text = (
                f"Wallet <b>active</b>: {_short(bw.get('address'))} — "
                "<b>key backup unconfirmed</b> (see below)."
            )
        else:
            colour = PROFIT_GREEN
            text = f"Wallet <b>active</b>: {_short(bw.get('address'))}"
        self._banner.setText(text)
        self._banner.setStyleSheet(
            f"""
            QLabel {{
                color: {colour};
                background-color: {PANEL_BG};
                border: 1px solid {colour};
                border-radius: 4px;
                padding: 10px 12px;
            }}
            """
        )

    def _render_notice(self, snap: dict) -> None:
        action_error = snap.get("wallet_action_error")
        notice = snap.get("wallet_notice")
        if action_error:
            self._notice.setText(f"❌  Last action failed: {action_error}")
            self._notice.setStyleSheet(f"color: {LOSS_RED};")
            self._notice.setVisible(True)
        elif notice:
            self._notice.setText(f"ℹ️  {notice}")
            self._notice.setStyleSheet(f"color: {INFO_BLUE};")
            self._notice.setVisible(True)
        else:
            self._notice.setVisible(False)

    # ------------------------------------------------------------------
    # Actions
    # ------------------------------------------------------------------

    def _emit_action(self, action: str, payload: dict) -> None:
        """Grey the controls until the worker processes THIS action, then emit."""
        self._action_pending = True
        self._pending_since_seq = self._last_seen_seq
        self._render()
        self.wallet_action_requested.emit(action, payload)

    def _on_create_clicked(self) -> None:
        self._emit_action("create", {})

    def _on_backup_clicked(self) -> None:
        self._emit_action("reveal_backup", {})

    def _on_send_clicked(self) -> None:
        """Parse, confirm with the exact amount + destination, then emit."""
        asset = str(self._send_asset.currentText())
        text = (self._send_amount.text() or "").strip()
        dest = (self._send_dest.text() or "").strip()
        decimals = 6 if asset == "USDC" else 18
        units = parse_asset_amount(text, decimals=decimals)
        if units is None:
            _log.warning("send: amount %r is not a valid %s amount", text, asset)
            self._send_amount.setFocus()
            return
        if not dest:
            self._send_dest.setFocus()
            return
        human = _exact_amount(units, decimals)
        answer = QMessageBox.question(
            self,
            "Confirm transfer",
            (
                f"Send {human} {asset} (Base network) to:\n\n{dest}\n\n"
                "The transfer is irreversible once broadcast."
            ),
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        if asset == "USDC":
            self._emit_action(
                "send_usdc", {"destination": dest, "amount_micros": units}
            )
        else:
            self._emit_action(
                "send_eth", {"destination": dest, "amount_wei": units}
            )

    def _on_convert_clicked(self) -> None:
        """Parse, floor to granularity, confirm with exact amounts, emit."""
        wrap = self._convert_dir.currentIndex() == 0  # ETH -> milliETH
        text = (self._convert_amount.text() or "").strip()
        if wrap:
            units = parse_asset_amount(text, decimals=18)
            if units is None or units <= 0:
                _log.warning("convert: amount %r is not a valid ETH amount",
                             text)
                self._convert_amount.setFocus()
                return
            # Floor to the contract's 1e12-wei granularity rather than
            # bouncing the operator for a sub-granule tail.
            units -= units % 10 ** 12
            if units <= 0:
                self._convert_amount.setFocus()
                return
            human_in = _exact_amount(units, 18)
            human_out = _millieth(units // 10 ** 12)
            prompt = (
                f"Wrap {human_in} ETH into {human_out} milliETH "
                "(MilliETH contract, no fee)?\n\nThe relay-gas minimum "
                "is always left untouched; the action is refused if the "
                "amount plus worst-case gas would dip below it."
            )
            action, payload = "wrap_eth", {"amount_wei": units}
        else:
            units = parse_asset_amount(text, decimals=3)
            if units is None or units <= 0:
                _log.warning(
                    "convert: amount %r is not a valid milliETH amount", text)
                self._convert_amount.setFocus()
                return
            human_in = _millieth(units)
            human_out = _exact_amount(units * 10 ** 12, 18)
            prompt = (
                f"Unwrap {human_in} milliETH back into {human_out} ETH "
                "(MilliETH contract, no fee)?"
            )
            action, payload = "unwrap_millieth", {"amount_units": units}
        answer = QMessageBox.question(
            self,
            "Confirm conversion",
            prompt,
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        self._emit_action(action, payload)

    def _on_rotate_clicked(self) -> None:
        """Confirm the full consequences, then request the rotation."""
        answer = QMessageBox.warning(
            self,
            "Rotate the hot-wallet key?",
            (
                "This will:\n\n"
                "  •  generate a fresh key and sweep ALL USDC, then all ETH\n"
                "      (minus gas), to its address;\n"
                "  •  archive the old key in secrets.yaml (never deleted);\n"
                "  •  CHANGE the deposit address — update exchange\n"
                "      allowlists; in-flight deposits to the old address land\n"
                "      at the archived key (recoverable via the runbook);\n"
                "  •  require a fresh key backup afterwards.\n\n"
                "It is refused while any warp bridge job is open.\n\n"
                "Rotate now?"
            ),
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        self._emit_action("rotate", {})

    def build_qr_dialog(self, address: str) -> "QDialog":
        """The receive-QR dialog: code, address, and the network warning.

        Factored from the click handler so offscreen tests can construct
        and inspect it without a modal exec().
        """
        from PySide6.QtGui import QPixmap
        from PySide6.QtWidgets import QDialog, QVBoxLayout

        dlg = QDialog(self)
        dlg.setWindowTitle("Receive on Base")
        v = QVBoxLayout(dlg)
        pix = QPixmap()
        pix.loadFromData(qr_png(address))
        qr_lbl = QLabel()
        qr_lbl.setObjectName("qr_image")
        qr_lbl.setPixmap(pix)
        qr_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v.addWidget(qr_lbl)
        addr_lbl = QLabel(address)
        addr_lbl.setObjectName("qr_address")
        addr_lbl.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        addr_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        addr_lbl.setStyleSheet("font-family: Consolas, monospace;")
        v.addWidget(addr_lbl)
        warn = QLabel(
            "<b>Base network only.</b> In your exchange app, choose "
            "<b>Base</b> as the withdrawal network before scanning. "
            "This wallet's tooling operates on Base; funds sent to this "
            "address over any other network will not appear here and "
            "need manual recovery."
        )
        warn.setObjectName("qr_warning")
        warn.setWordWrap(True)
        warn.setStyleSheet(f"color: {WARNING_YELLOW};")
        v.addWidget(warn)
        return dlg

    def _on_qr_clicked(self) -> None:
        address = (self._addr_field.text() or "").strip()
        if not address.startswith("0x"):
            _log.warning("QR requested with no wallet address present")
            return
        try:
            dlg = self.build_qr_dialog(address)
        except Exception as exc:  # noqa: BLE001 -- a QR must never crash the page
            _log.error("QR dialog failed: %s", exc)
            QMessageBox.warning(
                self, "QR unavailable",
                f"Could not render the QR code: {exc}",
            )
            return
        dlg.exec()

    def _on_copy_address(self) -> None:
        """Copy the Base address to the clipboard."""
        address = self._address.strip()
        if not address:
            return
        clipboard = QApplication.clipboard()
        if clipboard is not None:
            clipboard.setText(address)
        self.address_copy_requested.emit(address)
        _log.debug("Base wallet address copied to clipboard.")

    # ------------------------------------------------------------------
    # Button factories (same idiom as gui.widgets.warp)
    # ------------------------------------------------------------------

    @staticmethod
    def _small_button(label: str) -> QPushButton:
        btn = QPushButton(label)
        btn.setFixedWidth(70)
        btn.setStyleSheet(
            f"""
            QPushButton {{
                background-color: {PANEL_BG};
                color: {TEXT_PRIMARY};
                border: 1px solid {BORDER};
                border-radius: 3px;
                padding: 6px 10px;
            }}
            QPushButton:hover {{ border-color: {PRIMARY_GREEN}; color: {LIGHT_GREEN}; }}
            QPushButton:disabled {{ color: {TEXT_DISABLED}; border-color: {BORDER}; }}
            """
        )
        return btn

    @staticmethod
    def _primary_button(label: str) -> QPushButton:
        btn = QPushButton(label)
        btn.setMinimumWidth(160)
        btn.setStyleSheet(
            f"""
            QPushButton {{
                background-color: {PRIMARY_GREEN};
                color: {DARK_BG};
                border: 1px solid {LIGHT_GREEN};
                border-radius: 4px;
                padding: 8px 16px;
                font-weight: bold;
                font-size: 12px;
            }}
            QPushButton:hover {{ background-color: {LIGHT_GREEN}; }}
            QPushButton:disabled {{
                background-color: {ELEVATED_BG};
                color: {TEXT_DISABLED};
                border-color: {BORDER};
            }}
            """
        )
        return btn
