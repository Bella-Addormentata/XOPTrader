"""Warp Bridge page widget for XOPTrader GUI.

Provides a self-contained panel that explains the warp.green cross-chain
bridge (Ethereum ↔ Chia), displays the user's Chia receive address for
wUSDC.b, and offers a one-click launch of the official warp.green portal.

The tab is intentionally read-only in this release: no private-key material
is ever displayed or entered here (ISO/IEC 27001:2022).

ISO/IEC 5055  -- all public APIs carry type hints and docstrings.
ISO/IEC 25000 -- degrades gracefully when wallet data is unavailable.
"""

from __future__ import annotations

import logging
import webbrowser
from typing import Any, Final, Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QApplication,
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QScrollArea,
    QSizePolicy,
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
WARNING_YELLOW: Final[str] = _C.WARNING_YELLOW
INFO_BLUE: Final[str] = _C.INFO_BLUE

_WARP_PORTAL_URL: Final[str] = "https://www.warp.green"
_WARP_DOCS_URL: Final[str] = "https://docs.warp.green/developers/introduction"

_log = logging.getLogger(__name__)


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


class WarpWidget(QWidget):
    """Warp Bridge information and launch panel.

    Displays:
    * A brief explanation of the warp.green bridge.
    * Step-by-step instructions for bridging USDC → wUSDC.b on Chia.
    * The user's Chia wallet receive address (read-only, populated via
      :meth:`update_balances`).
    * Buttons to open the warp.green portal and documentation in a browser.

    Parameters
    ----------
    parent : QWidget | None
        Parent widget.
    """

    # Emitted when the user requests to copy the receive address.
    address_copy_requested = Signal(str)

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._chia_address: str = ""
        self._build_ui()

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def update_balances(
        self,
        wallet_balances: dict[str, Any],
        **_kwargs: Any,
    ) -> None:
        """Refresh the displayed Chia receive address from wallet data.

        Parameters
        ----------
        wallet_balances : dict
            Mapping of wallet-name → balance dict as forwarded by
            ``MainWindow._on_bridge_data``.  The XCH wallet entry is
            expected to carry a ``"receive_address"`` key.
        """
        address = ""
        for name, info in wallet_balances.items():
            if isinstance(info, dict):
                addr = info.get("receive_address") or info.get("address") or ""
                if addr and (name.upper() == "XCH" or not address):
                    address = addr
        if address and address != self._chia_address:
            self._chia_address = address
            self._addr_field.setText(address)
            self._addr_field.setCursorPosition(0)

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------

    def _build_ui(self) -> None:
        """Assemble the page layout."""
        # Outer scroll area so the content is usable on small screens.
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

        # ── Page title ────────────────────────────────────────────────
        title = QLabel("🌉  Warp Bridge")
        title_font = title.font()
        title_font.setPointSize(16)
        title_font.setBold(True)
        title.setFont(title_font)
        title.setStyleSheet(f"color: {TEXT_PRIMARY};")
        layout.addWidget(title)

        subtitle = _body_label(
            "Transfer USDC between Ethereum and Chia using the warp.green "
            "trustless cross-chain bridge.  Wrapped USDC on Chia is "
            "denominated as <b>wUSDC.b</b> and is the primary quote "
            "currency traded on XOPTrader."
        )
        subtitle.setTextFormat(Qt.TextFormat.RichText)
        layout.addWidget(subtitle)

        layout.addWidget(_separator())

        # ── How it works ──────────────────────────────────────────────
        layout.addWidget(_section_label("How the Warp Bridge Works"))

        how_text = (
            "warp.green is a decentralised, trustless bridge between "
            "Ethereum (and other EVM-compatible chains) and the Chia "
            "blockchain.  It locks your USDC in an Ethereum smart "
            "contract and mints an equivalent amount of wUSDC.b in your "
            "Chia wallet — no custodians, no sign-up required.\n\n"
            "The reverse operation (Chia → Ethereum) burns wUSDC.b and "
            "releases USDC on Ethereum, minus a small bridge fee."
        )
        layout.addWidget(_body_label(how_text))

        layout.addWidget(_separator())

        # ── Step-by-step instructions ─────────────────────────────────
        layout.addWidget(_section_label("Bridging USDC → wUSDC.b (Ethereum → Chia)"))

        steps_group = QGroupBox()
        steps_group.setStyleSheet(
            f"""
            QGroupBox {{
                background-color: {PANEL_BG};
                border: 1px solid {BORDER};
                border-radius: 4px;
                padding: 12px;
            }}
            """
        )
        steps_layout = QVBoxLayout(steps_group)
        steps_layout.setSpacing(8)

        steps = [
            ("1", "Open the warp.green portal (button below)."),
            ("2", "Connect your Ethereum wallet (MetaMask or WalletConnect)."),
            (
                "3",
                "Select <b>Ethereum → Chia</b> as the bridge direction and "
                "<b>USDC</b> as the asset.",
            ),
            (
                "4",
                "Paste your Chia receive address (shown below) into the "
                "destination field on the portal.",
            ),
            ("5", "Enter the USDC amount, review the fee estimate, and confirm."),
            (
                "6",
                "Wait for the Ethereum transaction to confirm (~15 confirmations). "
                "wUSDC.b will arrive in your Chia wallet automatically.",
            ),
        ]

        for step_num, step_text in steps:
            row = QHBoxLayout()
            row.setSpacing(10)

            num_lbl = QLabel(step_num)
            num_lbl.setFixedSize(24, 24)
            num_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
            num_lbl.setStyleSheet(
                f"""
                color: {DARK_BG};
                background-color: {PRIMARY_GREEN};
                border-radius: 12px;
                font-weight: bold;
                font-size: 11px;
                """
            )
            row.addWidget(num_lbl)

            step_lbl = QLabel(step_text)
            step_lbl.setWordWrap(True)
            step_lbl.setTextFormat(Qt.TextFormat.RichText)
            step_lbl.setStyleSheet(f"color: {TEXT_PRIMARY};")
            row.addWidget(step_lbl, 1)

            steps_layout.addLayout(row)

        layout.addWidget(steps_group)

        layout.addWidget(_separator())

        # ── Receive address ───────────────────────────────────────────
        layout.addWidget(_section_label("Your Chia Receive Address"))

        addr_note = _body_label(
            "Use this address as the destination when bridging USDC to Chia. "
            "It corresponds to your XCH wallet's current receive address."
        )
        layout.addWidget(addr_note)

        addr_row = QHBoxLayout()
        self._addr_field = QLineEdit()
        self._addr_field.setReadOnly(True)
        self._addr_field.setPlaceholderText(
            "Connect to the bot engine to load your wallet address …"
        )
        self._addr_field.setStyleSheet(
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
            QLineEdit:focus {{
                border-color: {PRIMARY_GREEN};
            }}
            """
        )
        addr_row.addWidget(self._addr_field, 1)

        copy_btn = QPushButton("Copy")
        copy_btn.setFixedWidth(70)
        copy_btn.setToolTip("Copy address to clipboard")
        copy_btn.setStyleSheet(
            f"""
            QPushButton {{
                background-color: {PANEL_BG};
                color: {TEXT_PRIMARY};
                border: 1px solid {BORDER};
                border-radius: 3px;
                padding: 6px 10px;
            }}
            QPushButton:hover {{
                border-color: {PRIMARY_GREEN};
                color: {LIGHT_GREEN};
            }}
            """
        )
        copy_btn.clicked.connect(self._on_copy_address)
        addr_row.addWidget(copy_btn)
        layout.addLayout(addr_row)

        layout.addWidget(_separator())

        # ── Action buttons ────────────────────────────────────────────
        layout.addWidget(_section_label("Launch Portal"))

        btn_row = QHBoxLayout()
        btn_row.setSpacing(12)

        portal_btn = self._make_action_button(
            "🌉  Open warp.green Portal",
            primary=True,
        )
        portal_btn.setToolTip(_WARP_PORTAL_URL)
        portal_btn.clicked.connect(lambda: self._open_url(_WARP_PORTAL_URL))
        btn_row.addWidget(portal_btn)

        docs_btn = self._make_action_button("📄  Developer Docs")
        docs_btn.setToolTip(_WARP_DOCS_URL)
        docs_btn.clicked.connect(lambda: self._open_url(_WARP_DOCS_URL))
        btn_row.addWidget(docs_btn)

        btn_row.addStretch(1)
        layout.addLayout(btn_row)

        # ── Info notice ───────────────────────────────────────────────
        notice_box = QFrame()
        notice_box.setFrameShape(QFrame.Shape.StyledPanel)
        notice_box.setStyleSheet(
            f"""
            QFrame {{
                background-color: {PANEL_BG};
                border: 1px solid {WARNING_YELLOW};
                border-radius: 4px;
                padding: 8px;
            }}
            """
        )
        notice_layout = QVBoxLayout(notice_box)
        notice_layout.setContentsMargins(12, 8, 12, 8)

        notice_lbl = QLabel(
            "⚠️  <b>Important:</b> Always verify the warp.green URL in "
            "your browser before connecting your wallet.  XOPTrader opens "
            "the official warp.green website but is not affiliated with or "
            "responsible for its operation."
        )
        notice_lbl.setWordWrap(True)
        notice_lbl.setTextFormat(Qt.TextFormat.RichText)
        notice_lbl.setStyleSheet(f"color: {WARNING_YELLOW}; background: transparent; border: none;")
        notice_layout.addWidget(notice_lbl)

        layout.addWidget(notice_box)

        layout.addStretch(1)

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _make_action_button(label: str, primary: bool = False) -> QPushButton:
        """Create a styled action button."""
        btn = QPushButton(label)
        btn.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        btn.setMinimumWidth(200)

        if primary:
            bg = PRIMARY_GREEN
            fg = DARK_BG
            hover_bg = LIGHT_GREEN
        else:
            bg = PANEL_BG
            fg = TEXT_PRIMARY
            hover_bg = ELEVATED_BG

        btn.setStyleSheet(
            f"""
            QPushButton {{
                background-color: {bg};
                color: {fg};
                border: 1px solid {PRIMARY_GREEN};
                border-radius: 4px;
                padding: 8px 16px;
                font-weight: bold;
                font-size: 12px;
            }}
            QPushButton:hover {{
                background-color: {hover_bg};
            }}
            QPushButton:pressed {{
                background-color: {ELEVATED_BG};
            }}
            """
        )
        return btn

    def _on_copy_address(self) -> None:
        """Copy the displayed address to the clipboard."""
        address = self._addr_field.text().strip()
        if not address:
            return
        clipboard = QApplication.clipboard()
        if clipboard is not None:
            clipboard.setText(address)
        self.address_copy_requested.emit(address)
        _log.debug("Warp receive address copied to clipboard.")

    @staticmethod
    def _open_url(url: str) -> None:
        """Open *url* in the system default browser."""
        try:
            webbrowser.open(url)
        except Exception:
            _log.warning("Failed to open URL: %s", url)
