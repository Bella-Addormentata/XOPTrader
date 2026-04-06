"""Custom status bar for XOPTrader main window.

Displays real-time PnL, spread, inventory ratio, block height,
memory usage, and UTC clock across three sections.

Compliant with:
    - ISO/IEC 5055  (bounded arithmetic, no truncation bugs in mojo math)
    - ISO/IEC 25000 (usability: color-coded profit/loss, legible sizing)
"""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Final, Optional

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QProgressBar,
    QStatusBar,
    QWidget,
)

from gui.utils import mojos_to_xch_float

# ---------------------------------------------------------------------------
# Theme constants -- sourced from the canonical CHIA palette singleton.
# ---------------------------------------------------------------------------
from gui.theme import COLORS as _C

PRIMARY_GREEN: Final[str] = _C.PRIMARY_GREEN
DARK_BG: Final[str] = _C.DARK_BG
PANEL_BG: Final[str] = _C.PANEL_BG
BORDER: Final[str] = _C.BORDER
TEXT_PRIMARY: Final[str] = _C.TEXT_PRIMARY
TEXT_SECONDARY: Final[str] = _C.TEXT_SECONDARY
PROFIT_GREEN: Final[str] = _C.PROFIT_GREEN
LOSS_RED: Final[str] = _C.LOSS_RED


class StatusBar(QStatusBar):
    """CHIA-branded status bar with live metric readouts.

    Sections
    --------
    Left   : Total PnL (colour-coded green/red)
    Centre : Spread (bps), inventory ratio mini-bar, block height
    Right  : Memory RSS, UTC clock

    Parameters
    ----------
    parent : QWidget | None
        Parent widget (typically the MainWindow).
    """

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setSizeGripEnabled(False)
        self._build_ui()
        self._apply_stylesheet()

        # Initialise with zeroed values so the bar is never blank.
        self.update_metrics(
            pnl_mojos=0,
            spread_bps=0.0,
            inventory_ratio=0.5,
            block_height=0,
        )
        self._refresh_clock()
        self._refresh_memory()

    # -- UI construction ----------------------------------------------------

    def _build_ui(self) -> None:
        """Create all label widgets and insert them into the status bar."""

        # -- Left section: PnL ---------------------------------------------
        self._pnl_label = QLabel("PnL: 0.0000 XCH")
        self._pnl_label.setMinimumWidth(200)
        self.addWidget(self._pnl_label, stretch=0)

        # -- Centre section: spread, inventory, block height ----------------
        centre_container = QWidget(self)
        centre_layout = QHBoxLayout(centre_container)
        centre_layout.setContentsMargins(12, 0, 12, 0)
        centre_layout.setSpacing(20)

        self._spread_label = QLabel("Spread: -- bps")
        self._spread_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        centre_layout.addWidget(self._spread_label)

        # Tiny inventory ratio progress bar
        self._inventory_bar = QProgressBar()
        self._inventory_bar.setRange(0, 100)
        self._inventory_bar.setValue(50)
        self._inventory_bar.setFixedWidth(100)
        self._inventory_bar.setFixedHeight(14)
        self._inventory_bar.setTextVisible(False)
        self._inventory_bar.setStyleSheet(
            f"""
            QProgressBar {{
                background-color: {DARK_BG};
                border: 1px solid {BORDER};
                border-radius: 3px;
            }}
            QProgressBar::chunk {{
                background-color: {PRIMARY_GREEN};
                border-radius: 2px;
            }}
            """
        )
        centre_layout.addWidget(self._inventory_bar)

        self._block_label = QLabel("Block: --")
        self._block_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        centre_layout.addWidget(self._block_label)

        self.addWidget(centre_container, stretch=1)

        # -- Right section: memory + clock ----------------------------------
        self._memory_label = QLabel("Mem: -- MB")
        self._memory_label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self.addPermanentWidget(self._memory_label)

        self._clock_label = QLabel("UTC --:--:--")
        self._clock_label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self._clock_label.setMinimumWidth(120)
        self.addPermanentWidget(self._clock_label)

    def _apply_stylesheet(self) -> None:
        """Apply CHIA dark-theme styling to the entire status bar."""
        self.setStyleSheet(
            f"""
            QStatusBar {{
                background-color: {PANEL_BG};
                border-top: 1px solid {BORDER};
                color: {TEXT_SECONDARY};
                font-size: 13px;
                padding: 4px 12px;
                min-height: 28px;
            }}
            QLabel {{
                color: {TEXT_SECONDARY};
                font-size: 13px;
                padding: 0 6px;
            }}
            """
        )

    # -- public refresh API -------------------------------------------------

    def update_metrics(
        self,
        pnl_mojos: int,
        spread_bps: float,
        inventory_ratio: float,
        block_height: int,
        xch_usd_rate: float = 0.0,
    ) -> None:
        """Push a new set of live metrics into the status bar.

        Parameters
        ----------
        pnl_mojos : int
            Cumulative PnL in mojos.  Positive = profit, negative = loss.
        spread_bps : float
            Current effective spread in basis points.
        inventory_ratio : float
            Inventory ratio in [0.0, 1.0] (0 = fully base, 1 = fully quote).
        block_height : int
            Latest known blockchain block height.
        xch_usd_rate : float
            Current XCH price in USD.  When > 0 a parenthesised USD
            equivalent is appended to the PnL readout.
        """
        # PnL -- colour-coded
        xch_value: float = mojos_to_xch_float(pnl_mojos)
        colour = PROFIT_GREEN if pnl_mojos >= 0 else LOSS_RED
        sign = "+" if pnl_mojos > 0 else ""
        pnl_text = f"PnL: {sign}{xch_value:.4f} XCH"
        if xch_usd_rate > 0:
            usd_value = xch_value * xch_usd_rate
            pnl_text += f" (${usd_value:+,.2f})"
        self._pnl_label.setText(pnl_text)
        self._pnl_label.setStyleSheet(f"color: {colour}; font-weight: bold; font-size: 13px;")

        # Spread
        self._spread_label.setText(f"Spread: {spread_bps:.0f} bps")

        # Inventory ratio (clamp to [0, 100] for the progress bar)
        clamped: int = max(0, min(100, int(inventory_ratio * 100)))
        self._inventory_bar.setValue(clamped)

        # Block height with thousands separator
        self._block_label.setText(f"Block: {block_height:,}")

    def refresh_clock_and_memory(self) -> None:
        """Convenience method called by the 1-second timer in MainWindow."""
        self._refresh_clock()
        self._refresh_memory()

    # -- internal helpers ---------------------------------------------------

    def _refresh_clock(self) -> None:
        """Update the UTC clock label."""
        utc_now: datetime = datetime.now(tz=timezone.utc)
        self._clock_label.setText(utc_now.strftime("UTC %H:%M:%S"))

    def _refresh_memory(self) -> None:
        """Update the memory (RSS) label.

        Uses platform-specific helpers with clearly separated Linux /
        macOS and Windows code paths.  Falls back to 0 MB if neither
        path succeeds rather than crashing the UI.

        ISO/IEC 5055 -- each platform import is in its own try/except
        so that a partial import on one OS cannot shadow the other.
        """
        rss_mb: float = 0.0

        # ----- Unix / macOS path (resource module) -------------------------
        try:
            import resource  # type: ignore[import-not-found]
            import sys as _sys

            usage = resource.getrusage(resource.RUSAGE_SELF)
            if _sys.platform == "darwin":
                # macOS: ru_maxrss is in bytes.
                rss_mb = usage.ru_maxrss / (1024 * 1024)
            else:
                # Linux: ru_maxrss is in kilobytes.
                rss_mb = usage.ru_maxrss / 1024
        except (ImportError, OSError):
            # Not on Unix -- try the Windows kernel32 approach.
            try:
                import ctypes
                import ctypes.wintypes

                class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
                    """Win32 PROCESS_MEMORY_COUNTERS structure."""

                    _fields_ = [
                        ("cb", ctypes.wintypes.DWORD),
                        ("PageFaultCount", ctypes.wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t),
                        ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t),
                        ("PeakPagefileUsage", ctypes.c_size_t),
                    ]

                pmc = PROCESS_MEMORY_COUNTERS()
                pmc.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS)
                kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
                psapi = ctypes.windll.psapi  # type: ignore[attr-defined]
                handle = kernel32.GetCurrentProcess()
                if psapi.GetProcessMemoryInfo(
                    handle, ctypes.byref(pmc), pmc.cb
                ):
                    rss_mb = pmc.WorkingSetSize / (1024.0 * 1024.0)
            except (ImportError, OSError):
                # Neither platform path available -- display zero.
                pass

        self._memory_label.setText(f"Mem: {rss_mb:.0f} MB")
