"""Base Wallet page widget for XOPTrader GUI.

A small operator surface for the app-controlled **Base-network hot wallet** --
the working intermediary between an exchange (e.g. Coinbase) and the warp
bridge: ETH and USDC come in from the exchange, the bridge spends them,
unwraps land back here, and the surplus goes out to the exchange.

The page offers exactly four operations, all executed by
:class:`gui.services.basewallet.BaseWallet` on the warp worker thread:

* **Create wallet** -- generate the first DPAPI-encrypted key (refused if one
  already exists; rotation is the only way to replace a key).
* **Receive** -- display + copy the wallet's single static address. One key is
  one address on EVM chains; a "fresh receive address" only ever comes from
  key rotation.
* **Send** -- transfer ETH or USDC to an external address (EIP-55 validated,
  worst-case gas reserved, balance-checked -- all re-validated by the
  backend).
* **Rotate key** -- new key, sweep USDC then ETH to it, archive the old blob
  in ``warp.retired_keys`` forever. Refused while any warp job is open.

The tab is fed by the warp snapshot forwarded through
``MainWindow._on_bridge_data`` (``data["warp"]["base_wallet"]`` plus the
``wallet_notice`` / ``wallet_action_error`` companions); it never reads key
material and never performs chain I/O itself. Actions are surfaced as one Qt
signal (``wallet_action_requested``) that the main window wires to
``WarpService.wallet_action``.

ISO/IEC 5055  -- all public APIs carry type hints and docstrings.
ISO/IEC 25000 -- degrades gracefully across all snapshot shapes (absent,
                 unconfigured, configured, error).
"""

from __future__ import annotations

import logging
from decimal import Decimal, InvalidOperation
from typing import Any, Final, Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
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
_LOW_GAS_WEI: Final[int] = 1_000_000_000_000_000  # 0.001 ETH -- warn below

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


def _short(value: Any, head: int = 10, tail: int = 6) -> str:
    """Middle-truncate a long hex/address string for compact display."""
    s = str(value or "")
    if len(s) <= head + tail + 1:
        return s
    return f"{s[:head]}…{s[-tail:]}"


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
    except InvalidOperation:
        return None
    if not amount.is_finite() or amount <= 0:
        return None
    # A magnitude bound: Decimal accepts 1e309 happily, and while the backend
    # balance check would refuse it anyway, an absurd amount should never even
    # reach the confirmation dialog.
    if amount > Decimal(10) ** 12:
        return None
    quantum = Decimal(1).scaleb(-decimals)
    if amount % quantum != 0:
        return None
    return int(amount.scaleb(decimals))


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

    #: (action, payload) -> ``WarpService.wallet_action``. Actions: "create",
    #: "confirm_backup", "send_eth", "send_usdc", "rotate".
    wallet_action_requested = Signal(str, dict)
    #: Informational; the widget also copies to the clipboard itself.
    address_copy_requested = Signal(str)

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._snap: dict[str, Any] = {}
        self._address: str = ""
        # One in-flight action at a time: buttons grey out on click and come
        # back with the next snapshot, so an impatient double-click cannot
        # queue a second broadcast.
        self._action_pending: bool = False
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
        self._action_pending = False
        if self.isVisible():
            self._render()

    def showEvent(self, event: Any) -> None:  # noqa: N802 -- Qt override
        """Repaint from the latest snapshot when the tab becomes visible."""
        super().showEvent(event)
        self._render()

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
                "Afterwards, back up the key per the runbook — DPAPI "
                "encryption is bound to this Windows user, so a machine "
                "failure without a backup destroys access to the funds."
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
        layout.addLayout(addr_row)

        bal_row = QHBoxLayout()
        bal_row.setSpacing(24)
        self._usdc_lbl = QLabel()
        self._usdc_lbl.setStyleSheet(f"color: {TEXT_PRIMARY};")
        self._eth_lbl = QLabel()
        self._eth_lbl.setStyleSheet(f"color: {TEXT_PRIMARY};")
        self._retired_lbl = QLabel()
        self._retired_lbl.setStyleSheet(f"color: {TEXT_SECONDARY};")
        bal_row.addWidget(self._usdc_lbl)
        bal_row.addWidget(self._eth_lbl)
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
        backup_lbl = QLabel(
            "⚠️  The key backup has not been confirmed. Back it up per the "
            "runbook (it is DPAPI-bound to this Windows user), then confirm:"
        )
        backup_lbl.setWordWrap(True)
        backup_lbl.setStyleSheet(f"color: {WARNING_YELLOW};")
        backup_layout.addWidget(backup_lbl, 1)
        self._backup_btn = self._small_button("Done")
        self._backup_btn.setFixedWidth(90)
        self._backup_btn.setToolTip(
            "Record that the key has been backed up offline"
        )
        self._backup_btn.clicked.connect(self._on_confirm_backup)
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
        else:
            self._usdc_lbl.setText("USDC: —")
            self._eth_lbl.setText("ETH (gas): —")
        retired = bw.get("retired_count") or 0
        self._retired_lbl.setText(
            f"Retired keys: {retired}" if retired else ""
        )

        wei = bw.get("eth_wei")
        low = configured and isinstance(wei, int) and wei < _LOW_GAS_WEI
        if low:
            self._gas_lbl.setText(
                "⚠️  Low gas: sends and rotation need ETH on Base for fees. "
                "Fund the wallet with ~0.005 ETH."
            )
        self._gas_lbl.setVisible(bool(low))

        self._backup_row.setVisible(
            configured and not bool(bw.get("backup_confirmed"))
        )
        self._backup_btn.setEnabled(not self._action_pending)

        can_act = configured and not self._action_pending
        self._send_btn.setEnabled(can_act)
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

    def _render_banner(
        self, snap: dict, bw: dict, *, configured: bool, error: Any
    ) -> None:
        if not snap:
            colour, text = TEXT_SECONDARY, "Connecting to the wallet service…"
        elif error:
            colour = WARNING_YELLOW
            text = f"Wallet <b>unavailable</b>: {error}"
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
        """Grey the controls until the next snapshot, then emit."""
        self._action_pending = True
        self._render()
        self.wallet_action_requested.emit(action, payload)

    def _on_create_clicked(self) -> None:
        self._emit_action("create", {})

    def _on_confirm_backup(self) -> None:
        self._emit_action("confirm_backup", {})

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
        human = _usdc(units) if asset == "USDC" else _eth(units)
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
