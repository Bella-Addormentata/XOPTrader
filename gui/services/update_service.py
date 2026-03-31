"""Background service to check for new XOPTrader releases on GitHub.

Uses the GitHub Releases API (unauthenticated, public repo) to compare
the running version against the latest published release tag.
"""

from __future__ import annotations

import logging
from typing import Final

from packaging.version import InvalidVersion, Version
from PySide6.QtCore import QObject, QThread, Signal, Slot

# ---------------------------------------------------------------------------
_log: logging.Logger = logging.getLogger(__name__)

_RELEASES_URL: Final[str] = (
    "https://api.github.com/repos/dorkmo/XOPTrader/releases/latest"
)
_HTTP_TIMEOUT_S: Final[float] = 10.0


# =========================================================================== #
#  Worker (runs on a dedicated QThread)                                        #
# =========================================================================== #


class _UpdateWorker(QObject):
    """Fetches the latest release tag from GitHub in a background thread."""

    #: Emitted with (latest_version, release_url) when a newer version exists.
    update_available = Signal(str, str)
    #: Emitted when the running version is already up-to-date.
    up_to_date = Signal()
    #: Emitted on network / parsing errors.
    check_failed = Signal(str)

    def __init__(self, current_version: str) -> None:
        super().__init__()
        self._current_version = current_version

    @Slot()
    def check(self) -> None:  # noqa: C901
        """Hit the GitHub API and compare versions."""
        try:
            import requests  # type: ignore[import-untyped]
        except ImportError as exc:
            self.check_failed.emit(f"Missing dependency: {exc}")
            return

        try:
            resp = requests.get(
                _RELEASES_URL,
                timeout=_HTTP_TIMEOUT_S,
                headers={"Accept": "application/vnd.github+json"},
            )
            resp.raise_for_status()
            data = resp.json()
        except requests.RequestException as exc:
            _log.warning("Update check failed: %s", exc)
            self.check_failed.emit(str(exc))
            return

        tag: str = data.get("tag_name", "")
        html_url: str = data.get("html_url", "")

        # Strip leading 'v' if present (e.g. "v0.1.5" → "0.1.5").
        version_str = tag.lstrip("v")

        try:
            latest = Version(version_str)
            current = Version(self._current_version)
        except InvalidVersion as exc:
            _log.warning("Could not parse version: %s", exc)
            self.check_failed.emit(f"Bad version string: {exc}")
            return

        if latest > current:
            _log.info("Update available: %s → %s", current, latest)
            self.update_available.emit(str(latest), html_url)
        else:
            _log.info("Already up-to-date (%s)", current)
            self.up_to_date.emit()


# =========================================================================== #
#  Public service (lives on the GUI thread)                                    #
# =========================================================================== #


class UpdateService(QObject):
    """Thin main-thread wrapper around :class:`_UpdateWorker`.

    Signals
    -------
    update_available(version, url)
        A newer release was found.
    up_to_date()
        The running version is the latest.
    check_failed(error)
        Something went wrong during the check.
    """

    update_available = Signal(str, str)
    up_to_date = Signal()
    check_failed = Signal(str)

    def __init__(self, current_version: str, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._thread = QThread(self)

        self._worker = _UpdateWorker(current_version)
        self._worker.moveToThread(self._thread)

        # Forward worker signals to the service (queued across threads).
        self._worker.update_available.connect(self.update_available)
        self._worker.up_to_date.connect(self.up_to_date)
        self._worker.check_failed.connect(self.check_failed)

        self._thread.start()

    # -- public API --------------------------------------------------------

    def check(self) -> None:
        """Request an update check (non-blocking)."""
        # QMetaObject.invokeMethod is the canonical way to call a slot on
        # another thread, but connecting a one-shot signal works too.
        # We simply invoke the worker slot via QTimer.singleShot on the
        # worker thread's event loop.
        from PySide6.QtCore import QMetaObject, Qt as QtNS

        QMetaObject.invokeMethod(
            self._worker, "check", QtNS.ConnectionType.QueuedConnection
        )

    def shutdown(self) -> None:
        """Gracefully stop the background thread."""
        self._thread.quit()
        self._thread.wait(3000)
