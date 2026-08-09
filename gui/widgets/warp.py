"""Warp Bridge page widget for XOPTrader GUI.

A live monitoring surface for the automatic USDC (Base) -> wUSDC.b (Chia)
bridge run in the background by :class:`gui.services.warp.service.WarpService`.
The operator simply sends USDC on the **Base** network to the app-controlled
hot-wallet address shown here; XOPTrader detects the deposit and performs the
entire bridge unattended (approve + ``bridgeToChia`` -> validator attestation ->
6-of-10 BLS signatures from Nostr -> Chia claim spend), and wUSDC.b -- the bot's
primary quote asset -- lands in its Chia wallet.

The tab is fed exclusively by the warp snapshot forwarded through
``MainWindow._on_bridge_data`` (``data["warp"]``); it never reads private-key
material and never performs chain I/O itself. Actions are surfaced as Qt signals
(``bridge_now_requested``, ``job_action_requested``) that the main window wires
to the service. Key generation / import and claim-by-hash recovery are operator
runbook steps (the committed service exposes no GUI method for them), so they are
intentionally absent here.

ISO/IEC 5055  -- all public APIs carry type hints and docstrings.
ISO/IEC 25000 -- degrades gracefully when warp data is unavailable (three
                 snapshot shapes: empty {}, disabled/blocked {enabled, error},
                 and the full built snapshot).
"""

from __future__ import annotations

import logging
import webbrowser
from typing import Any, Final, Optional

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QAction
from PySide6.QtWidgets import (
    QApplication,
    QAbstractItemView,
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMenu,
    QPushButton,
    QScrollArea,
    QTableWidget,
    QTableWidgetItem,
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
_USDC_MICROS: Final[int] = 1_000_000        # USDC has 6 decimals
_WUSDC_MOJOS_PER_UNIT: Final[int] = 1_000   # wUSDC.b CAT has 3 decimals
_WEI_PER_ETH: Final[float] = 1e18
_LOW_GAS_WEI: Final[int] = 1_000_000_000_000_000  # 0.001 ETH -- warn below this

# Job-status string constants (mirror gui.services.warp.jobs.JobStatus values,
# kept as local literals so the widget imports no heavy warp module at GUI boot).
_ST_COMPLETED: Final[str] = "COMPLETED"
_ST_FAILED: Final[str] = "FAILED"
_ST_CANCELLED: Final[str] = "CANCELLED"
_ST_DRY_RUN_OK: Final[str] = "DRY_RUN_OK"
_CANCELLABLE: Final[frozenset[str]] = frozenset(
    {"AWAITING_DEPOSIT", "DEPOSIT_SEEN", "APPROVING",
     # The one cancellable outbound state: pure reads, nothing moved yet.
     "UNWRAP_CHECKS"}
)

_STATUS_LABELS: Final[dict[str, str]] = {
    "AWAITING_DEPOSIT": "Awaiting deposit",
    "DEPOSIT_SEEN": "Deposit seen",
    "APPROVING": "Approving USDC",
    "BRIDGING": "Bridging (Base)",
    "BRIDGE_CONFIRMED": "Bridge confirmed",
    "MESSAGE_SENT": "Attested",
    "FUNDING_CLAIM": "Funding claim",
    "CLAIM_FUNDED": "Claim funded",
    "COLLECTING_SIGS": "Collecting signatures",
    "CLAIMING": "Claiming on Chia",
    "COMPLETED": "Completed",
    "FAILED": "Failed",
    "CANCELLED": "Cancelled",
    "DRY_RUN_OK": "Dry run OK",
    # Outbound (unwrap).
    "UNWRAP_CHECKS": "Unwrap: checking gates",
    "BURN_SENT": "Unwrap: burn sent",
    "BURNING": "Unwrap: burning",
    "COLLECTING_EVM_SIGS": "Unwrap: collecting signatures",
    "RELAYING": "Unwrap: relaying to Base",
}

_WARP_PORTAL_URL: Final[str] = "https://www.warp.green"
_WARP_DOCS_URL: Final[str] = "https://docs.warp.green/developers/introduction"

_log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Small label / layout helpers
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


def _mono_field(placeholder: str) -> QLineEdit:
    """Return a read-only monospace field for addresses / ids."""
    field = QLineEdit()
    field.setReadOnly(True)
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


# ---------------------------------------------------------------------------
# Value formatting (null-safe -- snapshots carry SQL NULLs and absent keys)
# ---------------------------------------------------------------------------

def _usdc(micros: Any) -> str:
    """Format USDC micro-units as a human amount, or an em dash when unknown."""
    try:
        return f"{int(micros) / _USDC_MICROS:,.2f}"
    except (TypeError, ValueError):
        return "—"


def _wusdc(mojos: Any) -> str:
    """Format wUSDC.b mojos (3 decimals) as a human amount."""
    try:
        return f"{int(mojos) / _WUSDC_MOJOS_PER_UNIT:,.3f}"
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


class WarpWidget(QWidget):
    """Live status panel for the automatic USDC (Base) -> wUSDC.b bridge.

    Displays, all sourced from ``data["warp"]``:

    * a status banner (disabled / blocked reason / dry run / active);
    * the Base hot-wallet deposit address + copy, and its USDC / ETH balances
      with a low-gas warning;
    * the bridge configuration (destination XCH address, auto-bridge state,
      min/max caps, wUSDC.b asset id) and a "Bridge now" button, plus warp.green
      portal and developer-docs links;
    * a jobs table with per-job BaseScan / SpaceScan links, error tooltips, and a
      Retry / Sweep / Cancel context menu.

    Parameters
    ----------
    parent : QWidget | None
        Parent widget.
    """

    # Emitted when the user copies an address (informational; the widget also
    # copies to the clipboard itself).
    address_copy_requested = Signal(str)
    # Emitted when the user clicks "Bridge now" -> WarpService.request_bridge().
    bridge_now_requested = Signal()
    # (amount_mojos, receiver_0x) -> WarpService.request_unwrap().
    unwrap_requested = Signal(int, str)
    # Emitted for a per-job action -> WarpService.job_action(job_id, action).
    # ``action`` is one of "retry", "cancel", "sweep".
    job_action_requested = Signal(int, str)

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._snap: dict[str, Any] = {}
        self._jobs_rows: list[dict[str, Any]] = []
        self._hot_address: str = ""
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
            ``data["warp"]`` is consumed (the ``WarpService`` snapshot); the call
            is null-safe if that key is missing or ``None``.
        """
        self._snap = dict((data or {}).get("warp") or {})
        # Hidden-tab render gating: store always, repaint only when visible; a
        # fresh render also runs on showEvent when the user switches to the tab.
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
        title = QLabel("\U0001F309  Warp Bridge")
        title_font = title.font()
        title_font.setPointSize(16)
        title_font.setBold(True)
        title.setFont(title_font)
        title.setStyleSheet(f"color: {TEXT_PRIMARY};")
        layout.addWidget(title)

        subtitle = _body_label(
            "XOPTrader bridges <b>USDC on Base</b> to <b>wUSDC.b</b> on Chia "
            "automatically via the warp.green trustless bridge. Send USDC on the "
            "Base network to the hot-wallet address below and the bot performs "
            "the entire bridge in the background — no step-by-step actions "
            "required. wUSDC.b is the primary quote currency traded on XOPTrader."
        )
        subtitle.setTextFormat(Qt.TextFormat.RichText)
        layout.addWidget(subtitle)

        # -- Status banner ---------------------------------------------
        self._banner = QLabel()
        self._banner.setWordWrap(True)
        self._banner.setTextFormat(Qt.TextFormat.RichText)
        self._banner.setMinimumHeight(40)
        layout.addWidget(self._banner)

        layout.addWidget(_separator())

        # -- Hot-wallet panel ------------------------------------------
        layout.addWidget(_section_label("Base Deposit Address (hot wallet)"))
        layout.addWidget(
            _body_label(
                "Send <b>USDC on the Base network</b> to this address. Do not send "
                "assets on any other chain. The bot keeps only a transient balance "
                "here — each deposit is bridged out automatically."
            )
        )

        addr_row = QHBoxLayout()
        self._addr_field = _mono_field(
            "Not configured — add warp.evm_private_key_dpapi to secrets.yaml"
        )
        addr_row.addWidget(self._addr_field, 1)
        self._copy_btn = self._small_button("Copy")
        self._copy_btn.setToolTip("Copy the Base deposit address to the clipboard")
        self._copy_btn.clicked.connect(self._on_copy_address)
        addr_row.addWidget(self._copy_btn)
        layout.addLayout(addr_row)

        bal_row = QHBoxLayout()
        bal_row.setSpacing(24)
        self._usdc_lbl = QLabel()
        self._usdc_lbl.setStyleSheet(f"color: {TEXT_PRIMARY};")
        self._eth_lbl = QLabel()
        self._eth_lbl.setStyleSheet(f"color: {TEXT_PRIMARY};")
        bal_row.addWidget(self._usdc_lbl)
        bal_row.addWidget(self._eth_lbl)
        bal_row.addStretch(1)
        layout.addLayout(bal_row)

        self._gas_lbl = QLabel()
        self._gas_lbl.setWordWrap(True)
        self._gas_lbl.setStyleSheet(f"color: {WARNING_YELLOW};")
        self._gas_lbl.setVisible(False)
        layout.addWidget(self._gas_lbl)

        layout.addWidget(_separator())

        # -- Bridge configuration + controls ---------------------------
        layout.addWidget(_section_label("Bridging USDC (Base) → wUSDC.b (Chia)"))

        dest_row = QHBoxLayout()
        dest_lead = QLabel("Destination (Chia):")
        dest_lead.setStyleSheet(f"color: {TEXT_SECONDARY};")
        dest_row.addWidget(dest_lead)
        self._dest_field = _mono_field(
            "Defaults to the bot's Chia wallet address"
        )
        dest_row.addWidget(self._dest_field, 1)
        layout.addLayout(dest_row)

        self._auto_lbl = _body_label("")
        layout.addWidget(self._auto_lbl)

        self._asset_lbl = QLabel()
        self._asset_lbl.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 11px;")
        self._asset_lbl.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        layout.addWidget(self._asset_lbl)

        ctrl_row = QHBoxLayout()
        self._bridge_btn = self._primary_button("\U0001F309  Bridge now")
        self._bridge_btn.clicked.connect(self._on_bridge_now)
        ctrl_row.addWidget(self._bridge_btn)
        ctrl_row.addStretch(1)

        # Deliberately always enabled, independent of `built`/`enabled`: the
        # portal is the documented out-of-band recovery path for a message this
        # app cannot claim, so a *blocked* operator is exactly who needs it.
        self._portal_btn = self._link_button("Open warp.green portal")
        self._portal_btn.setToolTip(
            f"{_WARP_PORTAL_URL}\n\nThe official bridge UI. Also the recovery path: "
            "an attested message stays claimable there forever, paid to the same "
            "attested Chia receiver.\n\nVerify the URL before connecting any wallet."
        )
        self._portal_btn.clicked.connect(lambda: _open_url(_WARP_PORTAL_URL))
        ctrl_row.addWidget(self._portal_btn)

        # -- Unwrap (Chia -> Base): burn wUSDC.b, release native USDC ------
        unwrap_row = QHBoxLayout()
        self._unwrap_amount = QLineEdit()
        self._unwrap_amount.setPlaceholderText("USDC amount (min 0.001)")
        self._unwrap_amount.setMaximumWidth(160)
        unwrap_row.addWidget(self._unwrap_amount)
        self._unwrap_dest = QLineEdit()
        # Deliberately NO default destination: the receiver is curried into
        # the burn puzzle, so a wrong address is unrecoverable by anyone.
        # The operator types it, every time.
        self._unwrap_dest.setPlaceholderText("Base destination (0x..., EIP-55)")
        unwrap_row.addWidget(self._unwrap_dest, 1)
        self._unwrap_btn = self._primary_button("🔥  Unwrap")
        self._unwrap_btn.setToolTip(
            "Burn wUSDC.b on Chia and release native USDC to the destination.\n"
            "Irreversible once the burn is sent. Costs the 0.3% warp tip plus\n"
            "the 0.001 XCH toll. Requires warp.max_unwrap_usdc to be set."
        )
        self._unwrap_btn.clicked.connect(self._on_unwrap_clicked)
        unwrap_row.addWidget(self._unwrap_btn)
        layout.addLayout(unwrap_row)

        self._docs_btn = self._link_button("Developer docs")
        self._docs_btn.setToolTip(_WARP_DOCS_URL)
        self._docs_btn.clicked.connect(lambda: _open_url(_WARP_DOCS_URL))
        ctrl_row.addWidget(self._docs_btn)
        layout.addLayout(ctrl_row)

        layout.addWidget(_separator())

        # -- Jobs table ------------------------------------------------
        layout.addWidget(_section_label("Bridge Jobs"))
        self._jobs_table = self._build_jobs_table()
        layout.addWidget(self._jobs_table)
        self._jobs_empty = _body_label("No bridge jobs yet.")
        layout.addWidget(self._jobs_empty)

        layout.addWidget(_separator())

        # -- Safety notice ---------------------------------------------
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
            "⚠️  <b>Important:</b> deposits must be <b>USDC on Base</b>. "
            "The hot-wallet key is stored encrypted (DPAPI) in secrets.yaml; back "
            "it up per the runbook. Bridging is disabled by default, and ships in "
            "dry run — go live only after a clean dry-run rehearsal followed by a "
            "small live test deposit."
        )
        notice_lbl.setWordWrap(True)
        notice_lbl.setTextFormat(Qt.TextFormat.RichText)
        notice_lbl.setStyleSheet(
            f"color: {WARNING_YELLOW}; background: transparent; border: none;"
        )
        notice_layout.addWidget(notice_lbl)
        layout.addWidget(notice_box)

        layout.addStretch(1)

    def _build_jobs_table(self) -> QTableWidget:
        """Create the read-only jobs table with a per-row action menu."""
        table = QTableWidget(0, 5)
        table.setHorizontalHeaderLabels(
            ["Status", "USDC in", "wUSDC.b out", "Updated", "Base tx"]
        )
        table.verticalHeader().setVisible(False)
        table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        table.setAlternatingRowColors(True)
        table.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        table.customContextMenuRequested.connect(self._on_jobs_menu)
        header = table.horizontalHeader()
        header.setStretchLastSection(True)
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(3, QHeaderView.ResizeMode.ResizeToContents)
        table.setMinimumHeight(160)
        return table

    # ------------------------------------------------------------------
    # Rendering
    # ------------------------------------------------------------------

    def _render(self) -> None:
        """Repaint every panel from ``self._snap`` (handles all snapshot shapes)."""
        snap = self._snap
        enabled = bool(snap.get("enabled"))
        built = "hot_wallet" in snap  # rich keys present only when the engine built
        error = snap.get("error")

        self._render_banner(snap, enabled=enabled, built=built, error=error)
        self._render_hot_wallet(snap.get("hot_wallet") or {}, built=built)
        self._render_bridge_config(snap, enabled=enabled, built=built)
        self._render_jobs(list(snap.get("jobs") or []))

    def _render_banner(
        self, snap: dict, *, enabled: bool, built: bool, error: Any
    ) -> None:
        badge = ""
        if not snap:
            colour, text = TEXT_SECONDARY, "Connecting to the warp service…"
        elif not enabled:
            colour = TEXT_SECONDARY
            text = (
                "Bridge <b>disabled</b>. Set <code>warp.enabled: true</code> in "
                "config.yaml to activate automatic background bridging. Rehearse "
                "with <code>warp.dry_run: true</code> before going live."
            )
            if error and error != "warp disabled":
                text += f"<br><span style='color:{WARNING_YELLOW};'>{error}</span>"
        elif not built or error:
            colour = WARNING_YELLOW
            reason = error or "the engine could not start (check dependencies / config)"
            text = f"Bridge enabled but <b>blocked</b>: {reason}"
        elif snap.get("dry_run"):
            # Signing but never broadcasting is a safe state, not a healthy one:
            # make it impossible to mistake a rehearsal for a working bridge.
            colour = WARNING_YELLOW
            badge = (
                f" <span style='color:{DARK_BG}; background-color:{WARNING_YELLOW}; "
                f"padding:1px 6px; border-radius:3px;'><b>DRY RUN</b></span>"
            )
            text = (
                "Bridge in <b>dry run</b>: both Base transactions are built and "
                "signed, then <b>not broadcast</b>. No funds move. Set "
                "<code>warp.dry_run: false</code> and restart to go live."
            )
        else:
            colour = PROFIT_GREEN
            network = snap.get("network") or "mainnet"
            auto = "on" if snap.get("auto_bridge") else "off"
            text = (
                f"Bridge <b>active</b> on {network}. Automatic bridging of "
                f"detected deposits is <b>{auto}</b>."
            )
        # A rejected Bridge now / Retry / Sweep is otherwise silent: the worker
        # swallows the exception so it cannot kill the thread, which left the
        # operator watching a click do nothing.
        action_error = snap.get("action_error")
        if action_error:
            text += (
                f"<br><span style='color:{WARNING_YELLOW};'>Last action failed: "
                f"{action_error}</span>"
            )
        self._banner.setText(text + badge)
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

    def _render_hot_wallet(self, hot: dict, *, built: bool) -> None:
        address = str(hot.get("address") or "")
        self._hot_address = address
        self._addr_field.setText(address)
        self._addr_field.setCursorPosition(0)
        self._copy_btn.setEnabled(bool(address))

        hot_err = hot.get("error")
        if not built:
            self._usdc_lbl.setText("USDC: —")
            self._eth_lbl.setText("ETH (gas): —")
            self._gas_lbl.setVisible(False)
            return
        if hot_err:
            self._usdc_lbl.setText("USDC: unavailable")
            self._eth_lbl.setText("ETH (gas): unavailable")
            self._gas_lbl.setText(f"⚠️  Balance unavailable: {hot_err}")
            self._gas_lbl.setVisible(True)
            return

        self._usdc_lbl.setText(f"USDC: <b>{_usdc(hot.get('usdc_micros'))}</b>")
        self._usdc_lbl.setTextFormat(Qt.TextFormat.RichText)
        self._eth_lbl.setText(f"ETH (gas): <b>{_eth(hot.get('eth_wei'))}</b>")
        self._eth_lbl.setTextFormat(Qt.TextFormat.RichText)

        wei = hot.get("eth_wei")
        low = isinstance(wei, int) and wei < _LOW_GAS_WEI
        if low:
            self._gas_lbl.setText(
                "⚠️  Low gas: this wallet needs a small amount of ETH on "
                "Base to pay for the approve + bridge transactions. Fund it with "
                "~0.005 ETH."
            )
        self._gas_lbl.setVisible(bool(low))

    def _render_bridge_config(self, snap: dict, *, enabled: bool, built: bool) -> None:
        dest = str(snap.get("receiver_address") or "")
        self._dest_field.setText(dest)
        self._dest_field.setCursorPosition(0)
        source = str(snap.get("receiver_source") or "")
        if dest and source == "wallet":
            self._dest_field.setToolTip(
                f"{dest}\n\nDerived from the bot's own Chia wallet because "
                "warp.chia_receiver_address is blank."
            )
        elif dest:
            self._dest_field.setToolTip(f"{dest}\n\nFrom warp.chia_receiver_address.")
        else:
            self._dest_field.setToolTip(str(source) or "")

        if built:
            lo = _usdc(snap.get("min_micros"))
            hi = _usdc(snap.get("max_micros"))
            auto_on = bool(snap.get("auto_bridge"))
            state = "enabled" if auto_on else "disabled (deposits wait for Bridge now)"
            self._auto_lbl.setText(
                f"Auto-bridge is <b>{state}</b>. Automatic bridging covers deposits "
                f"between <b>{lo}</b> and <b>{hi}</b> USDC; <b>Bridge now</b> "
                f"bridges any balance, up to <b>{hi}</b>."
            )
            self._auto_lbl.setTextFormat(Qt.TextFormat.RichText)
            asset = snap.get("expected_asset_id") or ""
            self._asset_lbl.setText(f"wUSDC.b asset id: {_short(asset, 12, 8)}")
            self._asset_lbl.setToolTip(str(asset))
        else:
            self._auto_lbl.setText("")
            self._asset_lbl.setText("")
            self._asset_lbl.setToolTip("")

        # Any row the service still reports as the active job owns the single
        # slot, FAILED included -- the store keeps a failed job open until the
        # operator resolves it. Second-guessing that here (by filtering out
        # terminal-looking statuses) enabled a button that request_bridge then
        # rejected, and the rejection was swallowed: a silently dead control.
        active = snap.get("active_job")
        active_open = bool(active)
        status = str((active or {}).get("status") or "")
        can_bridge = built and enabled and not active_open
        self._bridge_btn.setEnabled(can_bridge)
        if not built or not enabled:
            self._bridge_btn.setToolTip("Enable the bridge in config to request a bridge.")
        elif status == _ST_FAILED:
            self._bridge_btn.setToolTip(
                "The last bridge job failed and still holds the single job slot. "
                "Right-click it in the table: Retry re-attempts it, Sweep recovers "
                "its funding coin and closes it, and Abandon closes a job neither "
                "of those can resolve (the recovery details go to the audit log)."
            )
        elif active_open:
            self._bridge_btn.setToolTip(
                "A bridge job is already in progress; only one runs at a time."
            )
        else:
            self._bridge_btn.setToolTip(
                "Bridge the current Base USDC balance to wUSDC.b now."
            )

    def _render_jobs(self, jobs: list[dict]) -> None:
        self._jobs_rows = jobs
        table = self._jobs_table
        table.setRowCount(len(jobs))
        for row, job in enumerate(jobs):
            status = str(job.get("status") or "")
            status_item = QTableWidgetItem(_STATUS_LABELS.get(status, status or "—"))
            status_item.setForeground(_status_brush(status))
            last_error = job.get("last_error")
            if last_error:
                status_item.setToolTip(str(last_error))
            table.setItem(row, 0, status_item)

            table.setItem(row, 1, QTableWidgetItem(_usdc(job.get("amount_usdc_micros"))))
            out_mojos = job.get("post_tip_mojos") or job.get("amount_mojos")
            table.setItem(row, 2, QTableWidgetItem(_wusdc(out_mojos)))
            table.setItem(row, 3, QTableWidgetItem(str(job.get("updated_at") or "—")))

            tx = job.get("bridge_tx_hash")
            tx_item = QTableWidgetItem(_short(tx) if tx else "—")
            tip = []
            if tx:
                tip.append(f"tx: {tx}")
            if job.get("bridge_nonce"):
                tip.append(f"nonce: {job.get('bridge_nonce')}")
            if tip:
                tx_item.setToolTip("\n".join(tip))
            table.setItem(row, 4, tx_item)

        has_jobs = bool(jobs)
        self._jobs_table.setVisible(has_jobs)
        self._jobs_empty.setVisible(not has_jobs)

    # ------------------------------------------------------------------
    # Actions
    # ------------------------------------------------------------------

    def _on_unwrap_clicked(self) -> None:
        """Parse to mojos and emit; the engine re-validates everything."""
        from decimal import Decimal, InvalidOperation

        text = (self._unwrap_amount.text() or "").strip()
        dest = (self._unwrap_dest.text() or "").strip()
        # Decimal, not float: float(text) * 1000 rounds sub-minimum inputs UP
        # (0.0006 -> 1 mojo -- an unwrap the operator never typed), drifts on
        # values like 5.0005, and overflows on 1e309 with an exception the old
        # ValueError-only catch missed, crashing the click handler.
        try:
            amount = Decimal(text)
        except InvalidOperation:
            _log.warning("unwrap: amount %r is not a number", text)
            self._unwrap_amount.setFocus()
            return
        if not amount.is_finite() or amount % Decimal("0.001") != 0:
            _log.warning(
                "unwrap: amount %r is not a whole 0.001-USDC increment", text
            )
            self._unwrap_amount.setFocus()
            return
        mojos = int(amount * 1000)
        if mojos < 1:
            _log.warning("unwrap: amount %r is below the 0.001 USDC minimum", text)
            self._unwrap_amount.setFocus()
            return
        self.unwrap_requested.emit(mojos, dest)

    def _on_bridge_now(self) -> None:
        """Request an immediate bridge of the current Base USDC balance."""
        self.bridge_now_requested.emit()

    def _on_jobs_menu(self, pos: Any) -> None:
        """Show the Retry / Sweep / Cancel + explorer context menu for a job."""
        row = self._jobs_table.rowAt(pos.y())
        if row < 0 or row >= len(self._jobs_rows):
            return
        job = self._jobs_rows[row]
        job_id = job.get("id")
        status = str(job.get("status") or "")
        menu = QMenu(self)

        if status == _ST_FAILED and job_id is not None:
            act_retry = QAction("Retry", menu)
            act_retry.triggered.connect(
                lambda _=False, j=int(job_id): self.job_action_requested.emit(j, "retry")
            )
            menu.addAction(act_retry)
        if status in _CANCELLABLE and job_id is not None:
            act_cancel = QAction("Cancel", menu)
            act_cancel.triggered.connect(
                lambda _=False, j=int(job_id): self.job_action_requested.emit(j, "cancel")
            )
            menu.addAction(act_cancel)
        if status in (_ST_FAILED, _ST_COMPLETED) and job_id is not None:
            act_sweep = QAction("Sweep security coin", menu)
            act_sweep.triggered.connect(
                lambda _=False, j=int(job_id): self.job_action_requested.emit(j, "sweep")
            )
            menu.addAction(act_sweep)
        # Offered for FAILED, and for any job the engine is refusing outright.
        # A job frozen before the dry_run freeze (or against another hot wallet)
        # is read-only to the engine: Retry and Sweep raise on the binding
        # check, and a BRIDGING one is not cancellable either -- so its menu was
        # empty while it held the single active slot permanently.
        refused = bool(self._snap.get("error")) and self._snap.get("error") != "warp disabled"
        if (status == _ST_FAILED or refused) and job_id is not None:
            # The escape from a job the engine cannot resolve. A job that
            # tripped an attested-terms anchor has no ephemeral key, so Sweep
            # raises and Retry re-fails against the same attestation -- this
            # menu was empty for it, and FAILED holds the single active slot,
            # so the bridge could never open another job.
            act_abandon = QAction("Abandon job (frees the slot)", menu)
            act_abandon.triggered.connect(
                lambda _=False, j=int(job_id): self.job_action_requested.emit(j, "abandon")
            )
            menu.addAction(act_abandon)

        tx = job.get("bridge_tx_hash")
        receiver = job.get("receiver_address")
        if tx or receiver:
            if not menu.isEmpty():
                menu.addSeparator()
            if tx:
                url = self._basescan_url(str(tx))
                act_tx = QAction("Open in BaseScan", menu)
                act_tx.triggered.connect(lambda _=False, u=url: _open_url(u))
                menu.addAction(act_tx)
            if receiver:
                url = self._spacescan_url(str(receiver))
                act_rx = QAction("Open receiver in SpaceScan", menu)
                act_rx.triggered.connect(lambda _=False, u=url: _open_url(u))
                menu.addAction(act_rx)

        if not menu.isEmpty():
            menu.exec(self._jobs_table.viewport().mapToGlobal(pos))

    def _on_copy_address(self) -> None:
        """Copy the Base deposit address to the clipboard."""
        address = self._hot_address.strip()
        if not address:
            return
        clipboard = QApplication.clipboard()
        if clipboard is not None:
            clipboard.setText(address)
        self.address_copy_requested.emit(address)
        _log.debug("Warp Base deposit address copied to clipboard.")

    # ------------------------------------------------------------------
    # Explorer URL helpers
    # ------------------------------------------------------------------

    def _basescan_url(self, tx_hash: str) -> str:
        h = tx_hash if tx_hash.startswith("0x") else f"0x{tx_hash}"
        return f"https://basescan.org/tx/{h}"

    def _spacescan_url(self, address: str) -> str:
        return f"https://spacescan.io/address/{address}"

    # ------------------------------------------------------------------
    # Button factories
    # ------------------------------------------------------------------

    @staticmethod
    def _link_button(label: str) -> QPushButton:
        """A secondary button that opens an external URL.

        Distinct from :meth:`_small_button`, which pins a 70 px width suited to
        the inline Copy control and would truncate these labels.
        """
        btn = QPushButton(label)
        btn.setStyleSheet(
            f"""
            QPushButton {{
                background-color: {PANEL_BG};
                color: {TEXT_SECONDARY};
                border: 1px solid {BORDER};
                border-radius: 3px;
                padding: 8px 14px;
            }}
            QPushButton:hover {{ border-color: {PRIMARY_GREEN}; color: {LIGHT_GREEN}; }}
            """
        )
        return btn

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


def _status_brush(status: str):
    """Return the foreground brush colour for a job status."""
    from PySide6.QtGui import QBrush, QColor

    if status == _ST_COMPLETED:
        return QBrush(QColor(PROFIT_GREEN))
    if status == _ST_FAILED:
        return QBrush(QColor(LOSS_RED))
    if status == _ST_CANCELLED:
        return QBrush(QColor(TEXT_SECONDARY))
    if status == _ST_DRY_RUN_OK:
        return QBrush(QColor(WARNING_YELLOW))
    return QBrush(QColor(INFO_BLUE))


def _open_url(url: str) -> None:
    """Open *url* in the system default browser."""
    try:
        webbrowser.open(url)
    except Exception:  # noqa: BLE001 -- a failed browser launch must not crash the GUI
        _log.warning("Failed to open URL: %s", url)
