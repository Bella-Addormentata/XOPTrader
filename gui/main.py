#!/usr/bin/env python3
"""XOPTrader GUI -- CHIA DEX Market-Maker Control Panel.

Entry-point script that wires together argument parsing, the
QApplication, the main window, and service initialisation.

Usage
-----
    python -m gui.main --config config.yaml --db state.sqlite
    python -m gui.main --dry-run --font-size 2
"""

from __future__ import annotations

import argparse
import logging
import signal
import sys
from pathlib import Path
from typing import Optional

from gui.app import XOPTraderApp
from gui.services.engine_bridge import EngineBridge

# Module-level logger.
_log: logging.Logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def _build_parser() -> argparse.ArgumentParser:
    """Construct the CLI argument parser.

    Returns
    -------
    argparse.ArgumentParser ready for ``parse_args()``.
    """
    parser = argparse.ArgumentParser(
        prog="xoptrader-gui",
        description="XOPTrader -- CHIA DEX Market-Maker Control Panel",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        metavar="PATH",
        help="Path to the YAML configuration file.",
    )
    parser.add_argument(
        "--db",
        type=Path,
        default=None,
        metavar="PATH",
        help="Path to the SQLite state database.",
    )
    parser.add_argument(
        "--prometheus-url",
        type=str,
        default=None,
        metavar="URL",
        help="Base URL of the Prometheus metrics endpoint.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        default=False,
        help="Launch in read-only mode (no orders placed).",
    )
    parser.add_argument(
        "--font-size",
        type=int,
        default=0,
        metavar="DELTA",
        help=(
            "Font-size offset for accessibility.  "
            "+2 enlarges all text by 2 pt; -1 shrinks by 1 pt."
        ),
    )
    return parser


# ---------------------------------------------------------------------------
# Signal handling
# ---------------------------------------------------------------------------

def _install_signal_handlers(app: XOPTraderApp) -> None:
    """Register POSIX signal handlers for graceful shutdown.

    SIGINT (Ctrl-C) and SIGTERM cause the event loop to quit
    cleanly so that destructors and ``aboutToQuit`` slots run.

    Parameters
    ----------
    app:
        The running QApplication instance.
    """
    def _shutdown_handler(signum: int, _frame: object) -> None:
        """Receive a termination signal and request a clean exit."""
        sig_name = signal.Signals(signum).name
        sys.stderr.write(
            f"[XOPTrader] Received {sig_name} -- shutting down.\n"
        )
        app.quit()

    # SIGINT / SIGTERM are available on all POSIX platforms.
    signal.signal(signal.SIGINT, _shutdown_handler)
    signal.signal(signal.SIGTERM, _shutdown_handler)


# ---------------------------------------------------------------------------
# Service bootstrap
# ---------------------------------------------------------------------------

def _start_services(
    app: XOPTraderApp,
    config_path: Optional[Path],
    db_path: Optional[Path],
    prometheus_url: Optional[str],
    dry_run: bool,
) -> EngineBridge:
    """Create and initialise the EngineBridge service layer.

    The bridge owns ``ConfigService``, ``MetricsService``, and
    ``DatabaseService`` and runs their startup sequence (config load ->
    metrics connect -> DB open).

    Parameters
    ----------
    app:
        The running QApplication (used as the bridge's Qt parent so
        it is destroyed when the application exits).
    config_path:
        YAML configuration file, or *None* for built-in defaults.
    db_path:
        SQLite database file, or *None* to skip persistence.
    prometheus_url:
        Prometheus scrape endpoint, or *None* to disable metrics.
    dry_run:
        When *True*, the engine will not submit live orders.

    Returns
    -------
    EngineBridge
        The fully initialised bridge instance.
    """
    bridge = EngineBridge(
        config_path=config_path,
        db_path=db_path,
        metrics_url=prometheus_url,
        parent=app,
    )

    # Register graceful shutdown when the application is about to quit.
    app.aboutToQuit.connect(bridge.shutdown)

    # Run the startup sequence (config -> metrics -> DB -> ready).
    try:
        bridge.initialise()
    except Exception:
        _log.exception("Service layer initialisation failed.")
        from PySide6.QtWidgets import QMessageBox  # noqa: WPS433

        QMessageBox.critical(
            None,
            "XOPTrader — Startup Error",
            "Failed to initialise backend services.\n"
            "Check the log output for details.",
        )
        sys.exit(1)

    _log.info("Service layer initialised (dry_run=%s).", dry_run)
    return bridge


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    """Parse arguments, create the application, and enter the event loop."""
    args = _build_parser().parse_args()

    # Build the Qt application with the user's font-size preference.
    app = XOPTraderApp(
        argv=sys.argv,
        font_size_delta=args.font_size,
    )

    # Register OS signal handlers for orderly shutdown.
    _install_signal_handlers(app)

    # Import MainWindow here (not at module level) to keep the import
    # graph acyclic and to defer heavy widget instantiation until
    # the QApplication exists.
    from gui.widgets.main_window import MainWindow  # noqa: WPS433

    window = MainWindow(dry_run=args.dry_run)

    # Wire up background services (config, metrics, database).
    bridge = _start_services(
        app=app,
        config_path=args.config,
        db_path=args.db,
        prometheus_url=args.prometheus_url,
        dry_run=args.dry_run,
    )

    # Inject the bridge and wire all service signals to child widgets.
    window.set_bridge(bridge)

    # Show the main window and hand control to the Qt event loop.
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
