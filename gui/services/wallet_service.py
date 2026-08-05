"""Direct Chia wallet RPC client for querying wallet balances.

Queries the local Chia wallet daemon over its authenticated HTTPS RPC
interface to retrieve spendable, confirmed, and pending balances for
each wallet associated with the configured fingerprint.

All HTTP I/O runs in a dedicated ``QThread`` worker (mirroring
``MetricsService``) so the GUI event loop is never blocked -- a full
balance pass is log_in + get_wallets + one get_wallet_balance per
wallet, each with a 5 s timeout, which measured 4-11 s when the wallet
daemon is slow.

Compliant with:
    - ISO/IEC 27001:2022  (SSL certs loaded from disk, not embedded)
    - ISO/IEC 5055       (bounded timeout, deterministic error handling)
"""

from __future__ import annotations

import logging
import re
from pathlib import Path
from typing import Any, Final, Optional

from PySide6.QtCore import (
    QMutex,
    QMutexLocker,
    QObject,
    QThread,
    Signal,
    Slot,
)

_log: logging.Logger = logging.getLogger(__name__)

# Chia wallet RPC timeout (seconds).
_RPC_TIMEOUT_S: Final[float] = 5.0

# Mojo conversion factor (1 XCH = 1e12 mojos).
_MOJOS_PER_XCH: Final[float] = 1_000_000_000_000.0

# Well-known wallet type IDs from Chia.
_WALLET_TYPE_STANDARD: Final[int] = 0
_WALLET_TYPE_CAT: Final[int] = 6

# 64-char hex asset id (Chia CAT TAIL hash).  Used to extract an
# asset_id from the wallet's "data" field or its display name when the
# user hasn't renamed the wallet.
_ASSET_ID_RE: Final[re.Pattern[str]] = re.compile(r"[0-9a-fA-F]{64}")


def _params_from_config(config: dict[str, Any]) -> dict[str, Any]:
    """Extract wallet RPC connection parameters from a config snapshot."""
    chia = config.get("chia", {})
    return {
        "host": chia.get("wallet_host", "localhost"),
        "port": int(chia.get("wallet_port", 9256)),
        "fingerprint": chia.get("wallet_fingerprint"),
        "cert_path": str(chia.get("wallet_cert_path", "") or ""),
        "key_path": str(chia.get("wallet_key_path", "") or ""),
    }


# ===================================================================
# Worker -- runs blocking wallet RPC calls on a background QThread
# ===================================================================

class _WalletWorker(QObject):
    """Background worker performing the blocking wallet RPC sequence.

    This object is *moved* to a ``QThread`` and communicates with the
    main-thread ``WalletService`` exclusively through Qt signals, so
    the multi-second RPC pass never runs on the GUI thread.
    """

    # Emitted after every fetch attempt.  Carries the freshly fetched
    # wallet map on success or an empty dict on failure -- the service
    # merges into its cache and clears the in-flight guard either way.
    balances_ready = Signal(dict)

    def __init__(self, parent: Optional[QObject] = None) -> None:
        super().__init__(parent)
        self._host: str = "localhost"
        self._port: int = 9256
        self._fingerprint: Optional[int] = None
        self._cert_path: Path = Path("")
        self._key_path: Path = Path("")

    @Slot(dict)
    def set_params(self, params: dict) -> None:
        """Update connection parameters.

        Thread-safe: invoked via a queued signal from the main thread so
        the mutation occurs on the worker thread.
        """
        self._host = str(params.get("host", "localhost"))
        self._port = int(params.get("port", 9256))
        self._fingerprint = params.get("fingerprint")
        self._cert_path = Path(str(params.get("cert_path", ""))).expanduser()
        self._key_path = Path(str(params.get("key_path", ""))).expanduser()

    def _certs_available(self) -> bool:
        """Return True when both configured SSL cert files exist."""
        if self._cert_path.is_file() and self._key_path.is_file():
            return True
        _log.warning(
            "Wallet SSL certs not found: cert=%s key=%s",
            self._cert_path,
            self._key_path,
        )
        return False

    @Slot()
    def fetch(self) -> None:
        """Perform the blocking wallet RPC pass and emit ``balances_ready``.

        This slot is invoked from the main thread via a queued
        connection, so it executes on the worker thread.  Emits an empty
        dict on any failure so the service can clear its in-flight flag.
        """
        try:
            result = self._fetch_impl()
        except Exception as exc:  # noqa: BLE001 -- never kill the worker
            _log.warning("Wallet balance fetch failed unexpectedly: %s", exc)
            result = {}
        self.balances_ready.emit(result)

    def _fetch_impl(self) -> dict[str, dict[str, float]]:
        """Query the Chia wallet RPC and return per-wallet balances.

        Returns
        -------
        dict[str, dict[str, float]]
            Mapping of wallet name to ``{spendable, confirmed,
            pending_change, unconfirmed}`` in display units (XCH or
            token units).  Empty on failure.
        """
        try:
            import requests  # type: ignore[import-untyped]
            import urllib3
            urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
        except ImportError:
            _log.debug("requests library not available")
            return {}

        if not self._certs_available():
            return {}

        base_url = f"https://{self._host}:{self._port}"

        # Step 1: Log in with fingerprint (required for wallet RPC).
        if self._fingerprint:
            try:
                requests.post(
                    f"{base_url}/log_in",
                    json={"fingerprint": self._fingerprint},
                    cert=(str(self._cert_path), str(self._key_path)),
                    verify=False,
                    timeout=_RPC_TIMEOUT_S,
                )
            except requests.RequestException:
                pass  # Login may already be active.

        # Step 2: Get list of wallets.
        try:
            resp = requests.post(
                f"{base_url}/get_wallets",
                json={},
                cert=(str(self._cert_path), str(self._key_path)),
                verify=False,
                timeout=_RPC_TIMEOUT_S,
            )
            resp.raise_for_status()
            wallets_data = resp.json()
        except requests.RequestException as exc:
            _log.debug("Failed to get wallets: %s", exc)
            return {}

        if not wallets_data.get("success"):
            _log.debug("get_wallets returned success=false")
            return {}

        wallets = wallets_data.get("wallets", [])
        result: dict[str, dict[str, float]] = {}

        # Step 3: Query balance for each wallet.
        for wallet in wallets:
            wallet_id = wallet.get("id")
            wallet_name = wallet.get("name", f"Wallet {wallet_id}")
            wallet_type = wallet.get("type", _WALLET_TYPE_STANDARD)
            wallet_data_field = str(wallet.get("data", "") or "")

            if wallet_id is None:
                continue

            try:
                bal_resp = requests.post(
                    f"{base_url}/get_wallet_balance",
                    json={"wallet_id": wallet_id},
                    cert=(str(self._cert_path), str(self._key_path)),
                    verify=False,
                    timeout=_RPC_TIMEOUT_S,
                )
                bal_resp.raise_for_status()
                bal_data = bal_resp.json()
            except requests.RequestException as exc:
                _log.debug("Failed to get balance for wallet %s: %s", wallet_id, exc)
                continue

            if not bal_data.get("success"):
                continue

            balance = bal_data.get("wallet_balance", {})

            # For CAT wallets, resolve the on-chain asset id so callers
            # can map the wallet to a pair config regardless of the
            # user-assigned wallet name (Chia defaults to "CAT abcd...").
            # Try, in order: the "data" field returned by get_wallets,
            # a 64-char hex token embedded in the wallet's display name,
            # then the dedicated cat_get_asset_id RPC as a last resort.
            asset_id: str = ""
            if wallet_type == _WALLET_TYPE_CAT:
                m = _ASSET_ID_RE.search(wallet_data_field)
                if m:
                    asset_id = m.group(0).lower()
                if not asset_id:
                    m = _ASSET_ID_RE.search(wallet_name)
                    if m:
                        asset_id = m.group(0).lower()
                if not asset_id:
                    try:
                        aid_resp = requests.post(
                            f"{base_url}/cat_get_asset_id",
                            json={"wallet_id": wallet_id},
                            cert=(str(self._cert_path), str(self._key_path)),
                            verify=False,
                            timeout=_RPC_TIMEOUT_S,
                        )
                        aid_resp.raise_for_status()
                        aid_data = aid_resp.json()
                        if aid_data.get("success"):
                            asset_id = str(
                                aid_data.get("asset_id", "") or ""
                            ).lower()
                    except requests.RequestException as exc:
                        _log.debug(
                            "cat_get_asset_id failed for wallet %s: %s",
                            wallet_id, exc,
                        )
                if not asset_id:
                    _log.warning(
                        "Could not resolve asset_id for CAT wallet %s (%r); "
                        "target-allocation row will be missing",
                        wallet_id, wallet_name,
                    )

            # Convert mojos to display units for standard (XCH) wallets.
            # CAT wallets use 1000 mojos per unit.
            if wallet_type == _WALLET_TYPE_STANDARD:
                divisor = _MOJOS_PER_XCH
            else:
                divisor = 1000.0

            # `or 0` guards a key that is PRESENT but JSON-null in the wallet
            # RPC response: `.get(key, 0)` returns None in that case, and
            # float(None) raises TypeError.
            confirmed = float(balance.get("confirmed_wallet_balance") or 0) / divisor
            spendable = float(balance.get("spendable_balance") or 0) / divisor
            pending = float(balance.get("pending_change") or 0) / divisor
            unconfirmed = float(balance.get("unconfirmed_wallet_balance") or 0) / divisor

            result[wallet_name] = {
                "confirmed": confirmed,
                "spendable": spendable,
                "pending_change": pending,
                "unconfirmed": unconfirmed,
                "wallet_id": float(wallet_id),
                "wallet_type": float(wallet_type),
                "asset_id": asset_id,
            }

        return result


# ===================================================================
# Main service -- lives on the GUI thread
# ===================================================================

class WalletService(QObject):
    """Queries Chia wallet RPC for balance information.

    The blocking RPC pass runs on a dedicated worker ``QThread``;
    ``fetch_balances()`` only *triggers* a fetch and returns the cached
    snapshot immediately.  Overlapping fetches are skipped (never
    queued) so a slow wallet daemon cannot build a request backlog.

    Parameters
    ----------
    config : dict
        The full config dict (must contain ``chia`` section with
        ``wallet_host``, ``wallet_port``, ``wallet_cert_path``,
        ``wallet_key_path``, ``wallet_fingerprint``).
    parent : QObject | None
        Optional Qt parent.

    Signals
    -------
    balances_updated(dict)
        Emitted on the GUI thread after every completed fetch with the
        merged balance cache (empty fetches keep the previous cache).
    """

    # -- Qt signals ---------------------------------------------------------
    balances_updated = Signal(dict)

    # -- Internal trigger signals (queued connections to worker thread) -----
    _trigger_fetch = Signal()
    _trigger_params = Signal(dict)

    def __init__(
        self,
        config: dict[str, Any],
        parent: Optional[QObject] = None,
    ) -> None:
        super().__init__(parent)
        self._mutex = QMutex()
        self._cached: dict[str, dict[str, float]] = {}
        self._fetch_in_flight: bool = False

        # -- Worker thread --------------------------------------------------
        self._thread: QThread = QThread(self)
        self._thread.setObjectName("WalletWorkerThread")

        self._worker: _WalletWorker = _WalletWorker()
        self._worker.moveToThread(self._thread)

        # Worker signals -> main-thread slots (auto == queued here).
        self._worker.balances_ready.connect(self._on_balances_ready)

        # Queued connections: emit trigger signals to invoke worker slots
        # on the worker thread rather than blocking the GUI thread.
        self._trigger_fetch.connect(self._worker.fetch)
        self._trigger_params.connect(self._worker.set_params)

        # Dispatch the initial parameters; delivered once the thread runs.
        self._trigger_params.emit(_params_from_config(config))

    # ===================================================================
    # Lifecycle
    # ===================================================================

    def start(self) -> None:
        """Start the background worker thread."""
        if self._thread.isRunning():
            _log.warning("WalletService.start() called but thread already running.")
            return
        _log.info("Starting WalletService worker thread.")
        self._thread.start()

    def stop(self) -> None:
        """Cleanly shut down the worker thread.

        Safe to call even if the service was never started.
        """
        _log.info("Stopping WalletService.")
        if self._thread.isRunning():
            self._thread.quit()
            if not self._thread.wait(5_000):
                _log.warning("WalletService worker thread did not exit in time.")

    # ===================================================================
    # Public API
    # ===================================================================

    def update_config(self, config: dict[str, Any]) -> None:
        """Re-read connection parameters from a new config snapshot."""
        self._trigger_params.emit(_params_from_config(config))

    def fetch_balances(self) -> dict[str, dict[str, float]]:
        """Trigger an asynchronous balance fetch on the worker thread.

        Non-blocking: returns the cached snapshot immediately.  If a
        fetch is already in flight the trigger is skipped entirely (no
        backlog is queued).  Fresh results arrive via
        :pyattr:`balances_updated` and are merged into the cache read by
        :meth:`get_balances`.

        Returns
        -------
        dict[str, dict[str, float]]
            The last known balance snapshot (possibly empty).
        """
        if self._fetch_in_flight:
            _log.debug("Wallet fetch already in flight; skipping trigger.")
            return self._get_cached()

        if not self._thread.isRunning():
            # Lazy safety net for callers that never invoked start().
            self.start()

        self._fetch_in_flight = True
        self._trigger_fetch.emit()
        return self._get_cached()

    def get_balances(self) -> dict[str, dict[str, float]]:
        """Return cached balances without making an RPC call."""
        return self._get_cached()

    # ===================================================================
    # Internal slots (GUI thread)
    # ===================================================================

    @Slot(dict)
    def _on_balances_ready(self, result: dict) -> None:
        """Merge a completed fetch into the cache and clear the guard."""
        self._fetch_in_flight = False
        if result:
            with QMutexLocker(self._mutex):
                # Merge new data into cache rather than replacing it.
                # This prevents a single timed-out wallet RPC from
                # erasing previously-fetched wallets from the display.
                self._cached.update(result)
        self.balances_updated.emit(self._get_cached())

    def _get_cached(self) -> dict[str, dict[str, float]]:
        """Return the last successful balance snapshot."""
        with QMutexLocker(self._mutex):
            return dict(self._cached)
