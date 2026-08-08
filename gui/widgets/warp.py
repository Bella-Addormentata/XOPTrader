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
_CANCELLABLE: Final[frozenset[str]] = frozenset(
    {"AWAITING_DEPOSIT", "DEPOSIT_SEEN", "APPROVING"}
)
_TERMINAL: Final[frozenset[str]] = frozenset({_ST_COMPLETED, _ST_FAILED, _ST_CANCELLED})

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
}

_WARP_PORTAL_URL: Final[str] = "https://www.warp.green"

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

    * a status banner (disabled / blocked reason / active, with a TESTNET badge);
    * the Base hot-wallet deposit address + copy, and its USDC / ETH balances
      with a low-gas warning;
    * the bridge configuration (destination XCH address, auto-bridge state,
      min/max caps, wUSDC.b asset id) and a "Bridge now" button;
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
            "it up per the runbook. Bridging is disabled by default — enable "
            "it only after completing the testnet drill."
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
        testnet = bool(snap.get("testnet"))

        self._render_banner(snap, enabled=enabled, built=built, error=error, testnet=testnet)
        self._render_hot_wallet(snap.get("hot_wallet") or {}, built=built)
        self._render_bridge_config(snap, enabled=enabled, built=built)
        self._render_jobs(list(snap.get("jobs") or []))

    def _render_banner(
        self, snap: dict, *, enabled: bool, built: bool, error: Any, testnet: bool
    ) -> None:
        badge = ""
        if testnet:
            badge = (
                f" <span style='color:{DARK_BG}; background-color:{WARNING_YELLOW}; "
                f"padding:1px 6px; border-radius:3px;'><b>TESTNET</b></span>"
            )
        if not snap:
            colour, text = TEXT_SECONDARY, "Connecting to the warp service…"
        elif not enabled:
            colour = TEXT_SECONDARY
            text = (
                "Bridge <b>disabled</b>. Set <code>warp.enabled: true</code> in "
                "config.yaml (and complete the testnet drill) to activate "
                "automatic background bridging."
            )
            if error and error != "warp disabled":
                text += f"<br><span style='color:{WARNING_YELLOW};'>{error}</span>"
        elif not built or error:
            colour = WARNING_YELLOW
            reason = error or "the engine could not start (check dependencies / config)"
            text = f"Bridge enabled but <b>blocked</b>: {reason}"
        else:
            colour = PROFIT_GREEN
            network = snap.get("network") or "mainnet"
            auto = "on" if snap.get("auto_bridge") else "off"
            text = (
                f"Bridge <b>active</b> on {network}. Automatic bridging of "
                f"detected deposits is <b>{auto}</b>."
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
        self._dest_field.setText(str(snap.get("receiver_address") or ""))
        self._dest_field.setCursorPosition(0)

        if built:
            lo = _usdc(snap.get("min_micros"))
            hi = _usdc(snap.get("max_micros"))
            auto_on = bool(snap.get("auto_bridge"))
            state = "enabled" if auto_on else "disabled (deposits wait for Bridge now)"
            self._auto_lbl.setText(
                f"Auto-bridge is <b>{state}</b>. Deposits between "
                f"<b>{lo}</b> and <b>{hi}</b> USDC are eligible."
            )
            self._auto_lbl.setTextFormat(Qt.TextFormat.RichText)
            asset = snap.get("expected_asset_id") or ""
            self._asset_lbl.setText(f"wUSDC.b asset id: {_short(asset, 12, 8)}")
            self._asset_lbl.setToolTip(str(asset))
        else:
            self._auto_lbl.setText("")
            self._asset_lbl.setText("")
            self._asset_lbl.setToolTip("")

        # A bridge can only be requested when the engine is built and no job is
        # already active (WarpService.request_bridge raises otherwise).
        active = snap.get("active_job")
        active_open = bool(active) and active.get("status") not in _TERMINAL
        can_bridge = built and enabled and not active_open
        self._bridge_btn.setEnabled(can_bridge)
        if not built or not enabled:
            self._bridge_btn.setToolTip("Enable the bridge in config to request a bridge.")
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
    # Explorer URL helpers (network-aware)
    # ------------------------------------------------------------------

    def _basescan_url(self, tx_hash: str) -> str:
        host = "sepolia.basescan.org" if self._snap.get("testnet") else "basescan.org"
        h = tx_hash if tx_hash.startswith("0x") else f"0x{tx_hash}"
        return f"https://{host}/tx/{h}"

    def _spacescan_url(self, address: str) -> str:
        prefix = "testnet11." if self._snap.get("testnet") else ""
        return f"https://{prefix}spacescan.io/address/{address}"

    # ------------------------------------------------------------------
    # Button factories
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


def _status_brush(status: str):
    """Return the foreground brush colour for a job status."""
    from PySide6.QtGui import QBrush, QColor

    if status == _ST_COMPLETED:
        return QBrush(QColor(PROFIT_GREEN))
    if status == _ST_FAILED:
        return QBrush(QColor(LOSS_RED))
    if status == _ST_CANCELLED:
        return QBrush(QColor(TEXT_SECONDARY))
    return QBrush(QColor(INFO_BLUE))


def _open_url(url: str) -> None:
    """Open *url* in the system default browser."""
    try:
        webbrowser.open(url)
    except Exception:  # noqa: BLE001 -- a failed browser launch must not crash the GUI
        _log.warning("Failed to open URL: %s", url)
