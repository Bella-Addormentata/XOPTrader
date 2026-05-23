"""Direct Chia wallet RPC client for querying wallet balances.

Queries the local Chia wallet daemon over its authenticated HTTPS RPC
interface to retrieve spendable, confirmed, and pending balances for
each wallet associated with the configured fingerprint.

Compliant with:
    - ISO/IEC 27001:2022  (SSL certs loaded from disk, not embedded)
    - ISO/IEC 5055       (bounded timeout, deterministic error handling)
"""

from __future__ import annotations

import logging
import re
import ssl
from pathlib import Path
from typing import Any, Final, Optional

from PySide6.QtCore import QMutex, QMutexLocker, QObject

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


class WalletService(QObject):
    """Queries Chia wallet RPC for balance information.

    Parameters
    ----------
    config : dict
        The full config dict (must contain ``chia`` section with
        ``wallet_host``, ``wallet_port``, ``wallet_cert_path``,
        ``wallet_key_path``, ``wallet_fingerprint``).
    parent : QObject | None
        Optional Qt parent.
    """

    def __init__(
        self,
        config: dict[str, Any],
        parent: Optional[QObject] = None,
    ) -> None:
        super().__init__(parent)
        self._mutex = QMutex()
        self._cached: dict[str, dict[str, float]] = {}
        self._update_config(config)

    def _update_config(self, config: dict[str, Any]) -> None:
        chia = config.get("chia", {})
        self._host: str = chia.get("wallet_host", "localhost")
        self._port: int = int(chia.get("wallet_port", 9256))
        self._fingerprint: Optional[int] = chia.get("wallet_fingerprint")

        cert = chia.get("wallet_cert_path", "")
        key = chia.get("wallet_key_path", "")
        self._cert_path: Path = Path(str(cert)).expanduser()
        self._key_path: Path = Path(str(key)).expanduser()

    def update_config(self, config: dict[str, Any]) -> None:
        """Re-read connection parameters from a new config snapshot."""
        self._update_config(config)

    def _build_ssl_context(self) -> Optional[ssl.SSLContext]:
        """Create an SSL context with the configured wallet certs."""
        if not self._cert_path.is_file() or not self._key_path.is_file():
            _log.warning(
                "Wallet SSL certs not found: cert=%s key=%s",
                self._cert_path,
                self._key_path,
            )
            return None
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.load_cert_chain(str(self._cert_path), str(self._key_path))
        return ctx

    def fetch_balances(self) -> dict[str, dict[str, float]]:
        """Query the Chia wallet RPC and return per-wallet balances.

        Returns
        -------
        dict[str, dict[str, float]]
            Mapping of wallet name to ``{spendable, confirmed,
            pending_change, unconfirmed}`` in display units (XCH or
            token units).
        """
        try:
            import requests  # type: ignore[import-untyped]
            import urllib3
            urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
        except ImportError:
            _log.debug("requests library not available")
            return self._get_cached()

        ssl_ctx = self._build_ssl_context()
        if ssl_ctx is None:
            return self._get_cached()

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
            return self._get_cached()

        if not wallets_data.get("success"):
            _log.debug("get_wallets returned success=false")
            return self._get_cached()

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

            confirmed = float(balance.get("confirmed_wallet_balance", 0)) / divisor
            spendable = float(balance.get("spendable_balance", 0)) / divisor
            pending = float(balance.get("pending_change", 0)) / divisor
            unconfirmed = float(balance.get("unconfirmed_wallet_balance", 0)) / divisor

            result[wallet_name] = {
                "confirmed": confirmed,
                "spendable": spendable,
                "pending_change": pending,
                "unconfirmed": unconfirmed,
                "wallet_id": float(wallet_id),
                "wallet_type": float(wallet_type),
                "asset_id": asset_id,
            }

        if result:
            with QMutexLocker(self._mutex):
                # Merge new data into cache rather than replacing it.
                # This prevents a single timed-out wallet RPC from
                # erasing previously-fetched wallets from the display.
                self._cached.update(result)

        return self._get_cached()

    def _get_cached(self) -> dict[str, dict[str, float]]:
        """Return the last successful balance snapshot."""
        with QMutexLocker(self._mutex):
            return dict(self._cached)

    def get_balances(self) -> dict[str, dict[str, float]]:
        """Return cached balances without making an RPC call."""
        return self._get_cached()
