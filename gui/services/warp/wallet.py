"""Thin Chia wallet RPC client for the warp claim path.

The bridge uses the operator's own Chia wallet for exactly two things: to learn
the bot's receive address (the default claim destination) and to fund each claim
by sending ``attested_amount + claim_fee`` XCH to a per-job ephemeral address
(which the claim bundle then spends as its security coin). Everything else on the
Chia side goes through coinset; this client is deliberately tiny.

Transport is injected (``poster``) so the state machine and its tests need no
wallet daemon; the default poster mirrors :mod:`gui.services.wallet_service` --
HTTPS with the wallet's client-cert pair and ``verify=False`` (Chia's RPC uses a
private self-signed CA).
"""

from __future__ import annotations

from typing import Any, Callable, Optional

# (url, json_body) -> parsed JSON dict.
Poster = Callable[[str, dict], dict]


class WalletError(RuntimeError):
    """A wallet RPC call failed at transport level or returned success=false."""


def wallet_params_from_config(config: Any) -> dict:
    """Extract the wallet RPC connection params from a config object.

    Mirrors :func:`gui.services.wallet_service._params_from_config` so the warp
    stack and the existing wallet service agree on host/port/cert resolution.
    """
    chia = getattr(config, "chia", None) or object()

    def _get(name: str, default: Any = None) -> Any:
        return getattr(chia, name, default)

    return {
        "host": _get("wallet_host") or "localhost",
        "port": int(_get("wallet_port") or 9256),
        "fingerprint": _get("wallet_fingerprint"),
        "cert_path": _get("wallet_cert_path") or "",
        "key_path": _get("wallet_key_path") or "",
    }


def _default_poster(cert: str, key: str, timeout: float) -> Poster:
    import requests  # lazy: only when a real network call is made
    import urllib3

    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
    session = requests.Session()

    def post(url: str, body: dict) -> dict:
        resp = session.post(
            url, json=body, cert=(cert, key), verify=False, timeout=timeout
        )
        resp.raise_for_status()
        return resp.json()

    return post


class WalletClient:
    """Minimal Chia wallet RPC client (address + funding + tx lookups)."""

    def __init__(
        self,
        params: dict,
        *,
        poster: Optional[Poster] = None,
        timeout: float = 5.0,
    ) -> None:
        self._host = params.get("host") or "localhost"
        self._port = int(params.get("port") or 9256)
        self._fingerprint = params.get("fingerprint")
        self._cert = str(params.get("cert_path") or "")
        self._key = str(params.get("key_path") or "")
        self._timeout = timeout
        self._base = f"https://{self._host}:{self._port}"
        self._poster = poster or _default_poster(self._cert, self._key, timeout)

    def _post(self, endpoint: str, body: dict) -> dict:
        url = f"{self._base}/{endpoint}"
        try:
            data = self._poster(url, body)
        except WalletError:
            raise
        except Exception as exc:  # noqa: BLE001 -- unify transport failures
            raise WalletError(f"{endpoint} request failed: {exc}") from exc
        if not isinstance(data, dict):
            raise WalletError(f"{endpoint}: unexpected response {type(data).__name__}")
        if not data.get("success", False):
            raise WalletError(f"{endpoint} failed: {data.get('error') or 'unknown'}")
        return data

    def log_in(self) -> dict:
        """Select the configured fingerprint on the wallet daemon.

        Idempotent server-side; a no-op when no fingerprint is configured (the
        daemon then uses whatever key is already logged in).
        """
        if not self._fingerprint:
            return {}
        return self._post("log_in", {"fingerprint": int(self._fingerprint)})

    def get_next_address(self, wallet_id: int = 1, *, new_address: bool = True) -> str:
        data = self._post(
            "get_next_address",
            {"wallet_id": int(wallet_id), "new_address": bool(new_address)},
        )
        address = data.get("address")
        if not address:
            raise WalletError("get_next_address returned no address")
        return str(address)

    def send_transaction(
        self,
        wallet_id: int,
        amount_mojos: int,
        address: str,
        *,
        fee_mojos: int = 0,
        memos: Optional[list[str]] = None,
    ) -> dict:
        """Send XCH; returns the daemon's transaction record (incl. ``transaction_id``).

        The caller is responsible for de-duplication (scan by destination puzzle
        hash before sending) -- this method issues one RPC and never retries, so
        a resumed job cannot double-fund by calling it again blindly.
        """
        body: dict[str, Any] = {
            "wallet_id": int(wallet_id),
            "amount": int(amount_mojos),
            "address": str(address),
            "fee": int(fee_mojos),
        }
        if memos:
            body["memos"] = list(memos)
        data = self._post("send_transaction", body)
        tx = data.get("transaction")
        return tx if isinstance(tx, dict) else data

    def get_transaction(self, transaction_id: str) -> dict:
        data = self._post("get_transaction", {"transaction_id": str(transaction_id)})
        return data.get("transaction") or {}

    def get_transactions(
        self,
        wallet_id: int = 1,
        *,
        start: int = 0,
        end: int = 50,
        reverse: bool = True,
    ) -> list[dict]:
        data = self._post(
            "get_transactions",
            {
                "wallet_id": int(wallet_id),
                "start": int(start),
                "end": int(end),
                "reverse": bool(reverse),
            },
        )
        return list(data.get("transactions") or [])
