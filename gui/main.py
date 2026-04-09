#!/usr/bin/env python3
"""XOPTrader GUI -- CHIA DEX Market-Maker Control Panel.

Entry-point script that wires together argument parsing, the
QApplication, the main window, and service initialisation.
"""

from __future__ import annotations

import argparse
import logging
import os
import platform
import shutil
import signal
import subprocess
import sys
from pathlib import Path
from typing import Final, Optional

from gui.app import XOPTraderApp
from gui.services.engine_bridge import EngineBridge

# Module-level logger.
_log: logging.Logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Singleton enforcement -- kill stale GUI and engine instances
# ---------------------------------------------------------------------------

def _kill_old_instances() -> None:
    """Terminate any previously-running GUI and engine processes.

    Ensures only one GUI and one engine run at a time, preventing
    double-posting offers, port conflicts, and wallet RPC contention.

    On Windows, uses WMI via ``Get-CimInstance`` to match on command line.
    On POSIX, reads ``/proc/<pid>/cmdline``.
    """
    current_pid = os.getpid()
    # On Windows, the venv launcher spawns a child python.exe, both with
    # the same command line.  Protect both from being killed.
    parent_pid = os.getppid()
    protected_pids = {current_pid, parent_pid}

    if platform.system() == "Windows":
        _kill_old_instances_win32(protected_pids)
    else:
        _kill_old_instances_posix(protected_pids)


def _kill_old_instances_win32(protected_pids: set[int]) -> None:
    """Windows implementation: kill old GUI python procs and engine exes."""
    killed = 0

    # --- Kill old GUI processes (python running gui.main) ----------------
    try:
        result = subprocess.run(
            [
                "powershell", "-NoProfile", "-Command",
                "Get-CimInstance Win32_Process -Filter "
                "\"Name = 'python.exe' OR Name = 'pythonw.exe'\" "
                "| Select-Object ProcessId,CommandLine "
                "| ConvertTo-Json -Compress",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        if result.returncode == 0 and result.stdout.strip():
            import json
            procs = json.loads(result.stdout)
            if isinstance(procs, dict):
                procs = [procs]
            for proc in procs:
                pid = proc.get("ProcessId", 0)
                cmd = proc.get("CommandLine") or ""
                if pid in protected_pids or pid == 0:
                    continue
                # Match processes running our GUI module.
                if "gui.main" in cmd or "gui\\main" in cmd:
                    try:
                        os.kill(pid, signal.SIGTERM)
                        killed += 1
                        _log.info(
                            "[Startup] Terminated old GUI process (PID %d)",
                            pid,
                        )
                    except OSError:
                        pass
    except Exception as exc:
        _log.warning("[Startup] GUI process scan failed: %s", exc)

    # --- Kill old PyInstaller-bundled GUI --------------------------------
    _kill_processes_by_name_win32(
        ["xoptrader-gui.exe", "xoptrader_gui.exe"],
        protected_pids,
        "GUI",
    )
    killed_count = killed  # track for summary

    # --- Kill old engine processes ---------------------------------------
    killed_count += _kill_processes_by_name_win32(
        ["xop_trader.exe"],
        protected_pids,
        "engine",
    )

    if killed_count > 0:
        # Brief pause so OS can release ports and file handles.
        import time
        time.sleep(2)
        _log.info(
            "[Startup] Terminated %d old instance(s) -- ports released",
            killed_count,
        )
    else:
        _log.info("[Startup] No old GUI or engine instances found")


def _kill_processes_by_name_win32(
    names: list[str],
    protected_pids: set[int],
    label: str,
) -> int:
    """Kill Windows processes matching any of the given executable names."""
    killed = 0
    try:
        result = subprocess.run(
            [
                "powershell", "-NoProfile", "-Command",
                "Get-Process | Select-Object Id,ProcessName "
                "| ConvertTo-Json -Compress",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        if result.returncode == 0 and result.stdout.strip():
            import json
            procs = json.loads(result.stdout)
            if isinstance(procs, dict):
                procs = [procs]
            name_set = {n.lower().replace(".exe", "") for n in names}
            for proc in procs:
                pid = proc.get("Id", 0)
                pname = (proc.get("ProcessName") or "").lower()
                if pid in protected_pids or pid == 0:
                    continue
                if pname in name_set:
                    try:
                        os.kill(pid, signal.SIGTERM)
                        killed += 1
                        _log.info(
                            "[Startup] Terminated old %s (PID %d)",
                            label,
                            pid,
                        )
                    except OSError:
                        pass
    except Exception as exc:
        _log.warning("[Startup] %s process scan failed: %s", label, exc)
    return killed


def _kill_old_instances_posix(protected_pids: set[int]) -> None:
    """POSIX implementation: scan /proc for old GUI and engine processes."""
    killed = 0
    proc_dir = Path("/proc")
    if not proc_dir.is_dir():
        return

    for entry in proc_dir.iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        if pid in protected_pids:
            continue
        try:
            cmdline = (entry / "cmdline").read_text().replace("\x00", " ")
        except OSError:
            continue

        is_gui = "gui.main" in cmdline or "gui/main" in cmdline
        is_bundled_gui = "xoptrader-gui" in cmdline or "xoptrader_gui" in cmdline
        is_engine = "xop_trader" in cmdline

        if is_gui or is_bundled_gui or is_engine:
            label = "GUI" if (is_gui or is_bundled_gui) else "engine"
            try:
                os.kill(pid, signal.SIGTERM)
                killed += 1
                _log.info(
                    "[Startup] Sent SIGTERM to old %s (PID %d)", label, pid
                )
            except OSError:
                pass

    if killed > 0:
        import time
        time.sleep(2)
        # Send SIGKILL to any stubborn survivors.
        for entry in proc_dir.iterdir():
            if not entry.name.isdigit():
                continue
            pid = int(entry.name)
            if pid in protected_pids:
                continue
            try:
                cmdline = (entry / "cmdline").read_text().replace("\x00", " ")
            except OSError:
                continue
            if any(
                s in cmdline
                for s in ("gui.main", "gui/main", "xoptrader-gui",
                           "xoptrader_gui", "xop_trader")
            ):
                try:
                    os.kill(pid, signal.SIGKILL)
                    _log.info("[Startup] Sent SIGKILL to PID %d", pid)
                except OSError:
                    pass
        _log.info(
            "[Startup] Terminated %d old instance(s)", killed
        )
    else:
        _log.info("[Startup] No old GUI or engine instances found")


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
# First-run configuration bootstrap
# ---------------------------------------------------------------------------

_DEFAULT_CONFIG_NAME: Final[str] = "config.yaml"
_EXAMPLE_CONFIG_NAME: Final[str] = "config.example.yaml"
_DEFAULT_SECRETS_NAME: Final[str] = "secrets.yaml"
_EXAMPLE_SECRETS_NAME: Final[str] = "secrets.example.yaml"


def _bootstrap_config(config_path: Optional[Path]) -> None:
    """Copy ``config.example.yaml`` → ``config.yaml`` on first run.

    When *config_path* is *None* or points to a non-existent file, this
    function searches for ``config.example.yaml`` in the same directory
    (or the current working directory) and copies it to the target so
    that the GUI can start with a sensible template on first launch.

    The copy is silent; a log message records what happened.  No error
    is raised if neither the target nor the example can be found; the
    service layer handles the missing-file case gracefully.

    Parameters
    ----------
    config_path:
        The path supplied via ``--config``, or *None* for the default.
    """
    target = Path(config_path or _DEFAULT_CONFIG_NAME).resolve()
    if target.is_file():
        return

    # Candidate locations for the example config.
    app_dir = Path(__file__).resolve().parent
    project_dir = app_dir.parent
    candidates = [
        target.parent / _EXAMPLE_CONFIG_NAME,
        Path.cwd() / _EXAMPLE_CONFIG_NAME,
        project_dir / _EXAMPLE_CONFIG_NAME,
        app_dir / _EXAMPLE_CONFIG_NAME,
    ]

    # PyInstaller one-file bundles extract files under _MEIPASS.
    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        candidates.append(Path(meipass) / _EXAMPLE_CONFIG_NAME)
    for example in candidates:
        if example.is_file():
            # Ensure the destination directory exists when --config points
            # to a path whose parent has not been created yet.
            try:
                target.parent.mkdir(parents=True, exist_ok=True)
            except OSError as exc:
                _log.warning(
                    "Could not create config directory %s: %s",
                    target.parent,
                    exc,
                )
                return

            try:
                # Use copyfile (content only, no metadata) because the
                # destination will contain credentials.
                shutil.copyfile(example, target)
            except OSError as exc:
                _log.warning("Could not copy example config: %s", exc)
                return

            _log.info(
                "First-run bootstrap: copied %s → %s. "
                "Open Settings to review your credentials.",
                example,
                target,
            )

            # Apply restrictive permissions separately so a chmod failure
            # never obscures a successful copy.
            try:
                os.chmod(target, 0o600)
            except NotImplementedError:
                # Windows does not support POSIX chmod; skip silently.
                pass
            except OSError as exc:
                _log.warning(
                    "Could not set permissions on config file %s: %s",
                    target,
                    exc,
                )
            return


def _bootstrap_config_info(config_path: Optional[Path]) -> tuple[Path, bool]:
    """Bootstrap config and report whether a first-run copy happened.

    Returns
    -------
    tuple[Path, bool]
        ``(resolved_config_path, created_from_template)``
    """
    target = Path(config_path or _DEFAULT_CONFIG_NAME).resolve()
    existed_before = target.is_file()
    _bootstrap_config(config_path)
    created = (not existed_before) and target.is_file()

    # Also bootstrap secrets.yaml from secrets.example.yaml if it doesn't exist.
    secrets_target = target.parent / _DEFAULT_SECRETS_NAME
    if not secrets_target.is_file():
        for candidate_dir in [target.parent, Path.cwd(), Path(__file__).resolve().parent.parent]:
            example = candidate_dir / _EXAMPLE_SECRETS_NAME
            if example.is_file():
                try:
                    shutil.copyfile(example, secrets_target)
                    os.chmod(secrets_target, 0o600)
                except (OSError, NotImplementedError):
                    pass
                _log.info("First-run: copied %s → %s", example, secrets_target)
                break

    return target, created


# ---------------------------------------------------------------------------
# Chia auto-detection
# ---------------------------------------------------------------------------

def _detect_chia_root() -> Optional[Path]:
    """Return the Chia mainnet root directory if it can be found on this machine.

    Checks the standard ``~/.chia/mainnet`` location; returns *None* if
    the SSL directory is absent (Chia not installed / different root).
    """
    candidate = Path.home() / ".chia" / "mainnet"
    if (candidate / "config" / "ssl").is_dir():
        return candidate
    return None


def _detect_chia_rpc_ports(chia_root: Path) -> dict[str, int]:
    """Read full_node and wallet RPC ports from the Chia config file.

    Falls back to XOPTrader defaults (8555 / 9256) if the Chia config
    cannot be read or parsed.

    Parameters
    ----------
    chia_root:
        The Chia mainnet root returned by :func:`_detect_chia_root`.

    Returns
    -------
    dict with keys ``full_node_port`` and ``wallet_port``.
    """
    defaults = {"full_node_port": 8555, "wallet_port": 9256}
    chia_cfg_path = chia_root / "config" / "config.yaml"
    if not chia_cfg_path.is_file():
        return defaults
    try:
        import yaml  # noqa: WPS433

        with open(chia_cfg_path, "r", encoding="utf-8") as fh:
            chia_cfg = yaml.safe_load(fh) or {}
        ports = {}
        fn = chia_cfg.get("full_node", {})
        if isinstance(fn.get("rpc_port"), int):
            ports["full_node_port"] = fn["rpc_port"]
        wlt = chia_cfg.get("wallet", {})
        if isinstance(wlt.get("rpc_port"), int):
            ports["wallet_port"] = wlt["rpc_port"]
        return {**defaults, **ports}
    except Exception:  # pragma: no cover
        return defaults


def _detect_wallet_fingerprint() -> Optional[int]:
    """Return the first wallet fingerprint reported by ``chia keys show``.

    Runs ``chia`` from PATH with a 5-second timeout.  Returns *None* on
    any error (not installed, no keys, timeout, parse failure).
    """
    try:
        result = subprocess.run(
            ["chia", "keys", "show"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        for line in result.stdout.splitlines():
            if "Fingerprint" in line:
                parts = line.split(":", 1)
                if len(parts) == 2 and parts[1].strip().isdigit():
                    return int(parts[1].strip())
    except Exception:
        pass
    return None


def _patch_chia_auto_detect(config_path: Path) -> bool:
    """Overwrite placeholder Chia connection values with auto-detected ones.

    Reads *config_path*, replaces ``wallet_fingerprint``, SSL cert/key
    paths, and ``verify_ssl`` with values discovered from the local
    Chia installation, then writes the file back.  Only patches fields
    that still hold their template placeholder values so that any
    manually-set values are left untouched.

    Parameters
    ----------
    config_path:
        Path to the YAML config file that was just bootstrapped.

    Returns
    -------
    bool
        *True* if at least one value was patched; *False* otherwise
        (Chia not found or all fields already customised).
    """
    try:
        import yaml  # noqa: WPS433
    except ImportError:
        _log.warning("PyYAML not available — skipping Chia auto-detect.")
        return False

    chia_root = _detect_chia_root()
    fingerprint = _detect_wallet_fingerprint()

    if chia_root is None and fingerprint is None:
        return False

    from gui.services.config_split import load_merged, split_and_save  # noqa: WPS433

    try:
        data = load_merged(config_path)
    except OSError as exc:
        _log.warning("Could not read config for auto-patching: %s", exc)
        return False

    chia_section = data.get("chia", {})
    patched = False
    ssl = chia_root / "config" / "ssl" if chia_root else None

    _PLACEHOLDER_FINGERPRINT = 1234567890

    # --- wallet fingerprint ---
    if fingerprint is not None and chia_section.get("wallet_fingerprint") == _PLACEHOLDER_FINGERPRINT:
        chia_section["wallet_fingerprint"] = fingerprint
        patched = True

    # --- RPC ports (read from Chia's own config.yaml) ---
    if chia_root is not None:
        detected_ports = _detect_chia_rpc_ports(chia_root)
        for port_key, port_val in detected_ports.items():
            # Only overwrite if the current value matches the template default.
            template_defaults = {"full_node_port": 8555, "wallet_port": 9256}
            if chia_section.get(port_key) == template_defaults.get(port_key):
                chia_section[port_key] = port_val
                if port_val != template_defaults.get(port_key):
                    patched = True  # Only flag as patched if value actually changed.

    if ssl is not None:
        # Map config key → relative path under the ssl directory.
        cert_map = {
            "ssl_cert_path":    ssl / "full_node" / "private_full_node.crt",
            "ssl_key_path":     ssl / "full_node" / "private_full_node.key",
            "wallet_cert_path": ssl / "wallet"    / "private_wallet.crt",
            "wallet_key_path":  ssl / "wallet"    / "private_wallet.key",
            "ca_cert_path":     ssl / "ca"         / "chia_ca.crt",
        }
        for key, resolved in cert_map.items():
            # Only replace tilde-style placeholder paths from the template.
            existing = str(chia_section.get(key, ""))
            if existing.startswith("~") and resolved.is_file():
                # Store as forward-slash string for cross-platform YAML.
                chia_section[key] = resolved.as_posix()
                patched = True

        # Localhost connections don't need SSL verification.
        # T8-14: Only override verify_ssl when the user has not explicitly
        # set it.  Persisting False when the host later changes to a
        # remote address would silently disable certificate validation.
        host = str(chia_section.get("full_node_host", "localhost"))
        if host in ("localhost", "127.0.0.1", "::1"):
            if "verify_ssl" not in chia_section:
                chia_section["verify_ssl"] = False
                patched = True

    if not patched:
        return False

    data["chia"] = chia_section
    try:
        split_and_save(config_path, data)
    except OSError as exc:
        _log.warning("Could not write auto-patched config: %s", exc)
        return False

    _log.info(
        "Auto-detected Chia settings written to %s "
        "(root=%s, fingerprint=%s).",
        config_path,
        chia_root,
        fingerprint,
    )
    return True


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

    # Configure basic logging early so _kill_old_instances can log.
    logging.basicConfig(
        level=logging.INFO,
        format="[%(asctime)s] [%(levelname)s] %(message)s",
    )

    # Enforce singleton: kill any old GUI and engine processes.
    _kill_old_instances()

    # Build the Qt application with the user's font-size preference.
    app = XOPTraderApp(
        argv=sys.argv,
        font_size_delta=args.font_size,
    )

    # Register OS signal handlers for orderly shutdown.
    _install_signal_handlers(app)

    # On first run, copy config.example.yaml → config.yaml so the GUI
    # can launch with a template the user can edit via the Settings panel.
    cfg_path, first_run_bootstrap = _bootstrap_config_info(args.config)

    # If a fresh config was just created, auto-fill Chia cert paths and
    # wallet fingerprint from the local Chia installation.
    chia_auto_patched = False
    if first_run_bootstrap:
        chia_auto_patched = _patch_chia_auto_detect(cfg_path)

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

    if first_run_bootstrap:
        # On first run, open the Settings page and show the setup wizard
        # so users can review auto-detected values and enter credentials
        # (e.g. Telegram) that cannot be discovered automatically.
        from gui.widgets.setup_wizard import FirstRunSetupDialog  # noqa: WPS433

        if hasattr(window, "open_settings_page"):
            window.open_settings_page()

        wizard = FirstRunSetupDialog(
            config_path=cfg_path,
            chia_detected=chia_auto_patched,
            parent=window,
        )
        wizard.exec()

    # Show the main window and hand control to the Qt event loop.
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
