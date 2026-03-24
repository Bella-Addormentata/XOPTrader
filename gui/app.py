"""Application wrapper for the XOPTrader GUI.

Creates and configures a QApplication with CHIA branding, high-DPI
support, and a global exception hook that logs to stderr and shows
a modal error dialog so crashes are never silently swallowed.
"""

from __future__ import annotations

import sys
import traceback
from typing import Optional

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication, QMessageBox

from gui.theme import apply_theme


class XOPTraderApp(QApplication):
    """QApplication subclass pre-configured for XOPTrader.

    Parameters
    ----------
    argv:
        Command-line arguments forwarded to QApplication.
    font_size_delta:
        Accessibility offset applied to all font sizes (default 0).
    """

    def __init__(
        self,
        argv: Optional[list[str]] = None,
        font_size_delta: int = 0,
    ) -> None:
        # Enable high-DPI scaling before the QApplication is created.
        # AA_EnableHighDpiScaling was removed in Qt 6.x (always on),
        # but we guard for older PySide6 builds that still expose it.
        if hasattr(Qt.ApplicationAttribute, "AA_EnableHighDpiScaling"):
            QApplication.setAttribute(
                Qt.ApplicationAttribute.AA_EnableHighDpiScaling, True
            )

        super().__init__(argv or sys.argv)

        # Application identity -- used by QSettings and window titles.
        self.setApplicationName("XOPTrader")
        self.setOrganizationName("XOP")
        self.setApplicationVersion("0.1.0")

        # Apply the CHIA dark theme (Fusion base + full QSS).
        apply_theme(self, font_size_delta=font_size_delta)

        # Install the global exception hook so that unhandled Python
        # exceptions surface as visible error dialogs rather than
        # vanishing into a closed terminal.
        self._install_exception_hook()

    # ------------------------------------------------------------------
    # Exception handling
    # ------------------------------------------------------------------

    def _install_exception_hook(self) -> None:
        """Replace ``sys.excepthook`` with a handler that logs the
        traceback to *stderr* and presents a non-blocking error dialog.
        """
        sys.excepthook = self._handle_exception

    @staticmethod
    def _handle_exception(
        exc_type: type[BaseException],
        exc_value: BaseException,
        exc_tb: object,
    ) -> None:
        """Global exception handler.

        Writes the full traceback to *stderr* for log capture, then
        shows a ``QMessageBox.critical`` dialog so the user is aware
        an error occurred even when running without a visible console.

        Parameters
        ----------
        exc_type:
            The exception class.
        exc_value:
            The exception instance.
        exc_tb:
            The traceback object.
        """
        # Format the traceback exactly as Python would natively.
        tb_text = "".join(
            traceback.format_exception(exc_type, exc_value, exc_tb)
        )
        # Always log to stderr -- systemd / journald will capture this.
        sys.stderr.write(f"[XOPTrader] Unhandled exception:\n{tb_text}")
        sys.stderr.flush()

        # Show a modal dialog if a QApplication is still running.
        app = QApplication.instance()
        if app is not None:
            QMessageBox.critical(
                None,
                "XOPTrader -- Unhandled Exception",
                (
                    "An unexpected error occurred.  Details have been "
                    "written to stderr.\n\n"
                    f"{exc_type.__name__}: {exc_value}"
                ),
            )
