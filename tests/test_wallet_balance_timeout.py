"""[DBX-BALANCE 2026-11-08] A slow wallet's balance read must not drop it.

On 2026-11-07 one CAT wallet (DBX, many coins) took 6.5-7 s to answer
get_wallet_balance on every call.  The GUI's wallet worker gave each read
5 s, so the wallet never reached the Wallet Balances tab, and the Target
Portfolio Allocation table computed every Current % without it.  These
tests drive _WalletWorker._fetch_impl against a stub ``requests`` module.
"""

from __future__ import annotations

import logging
import sys
import types
from pathlib import Path

import pytest

pytest.importorskip("PySide6")

from gui.services import wallet_service as ws  # noqa: E402


class _Resp:
    def __init__(self, payload: dict) -> None:
        self._payload = payload
        self.ok = True

    def raise_for_status(self) -> None:
        return None

    def json(self) -> dict:
        return self._payload


class _Timeout(Exception):
    pass


def _stub_requests(slow_wallet_seconds: float, fail_wallets: set[int] = frozenset()):
    """A ``requests`` stand-in: wallet 8 needs ``slow_wallet_seconds``."""
    calls: list[tuple[str, dict, float]] = []

    class RequestException(Exception):
        pass

    def post(url, json=None, cert=None, verify=None, timeout=None):  # noqa: A002
        endpoint = url.rsplit("/", 1)[-1]
        calls.append((endpoint, dict(json or {}), timeout))
        if endpoint == "log_in":
            return _Resp({"success": True})
        if endpoint == "get_sync_status":
            return _Resp({"success": True, "synced": True, "syncing": False})
        if endpoint == "get_wallets":
            return _Resp({"success": True, "wallets": [
                {"id": 1, "name": "Chia Wallet", "type": 0, "data": ""},
                {"id": 8, "name": "CAT db1a9020",
                 "type": 6, "data": "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20"},
            ]})
        if endpoint == "get_wallet_balance":
            wid = json["wallet_id"]
            if wid in fail_wallets:
                raise RequestException("connection reset")
            needed = slow_wallet_seconds if wid == 8 else 0.1
            if timeout is not None and needed > timeout:
                raise RequestException(f"Read timed out. (read timeout={timeout})")
            return _Resp({"success": True, "wallet_balance": {
                "confirmed_wallet_balance": 12_411_968 if wid == 8 else 164_945_722_000_000,
                "spendable_balance": 12_411_968 if wid == 8 else 34_691_380_000_000,
                "pending_change": 0,
                "unconfirmed_wallet_balance": 12_411_968 if wid == 8 else 164_945_722_000_000,
            }})
        raise AssertionError(f"unexpected endpoint {endpoint}")

    mod = types.SimpleNamespace(post=post, RequestException=RequestException)
    return mod, calls


def _worker(tmp_path: Path) -> "ws._WalletWorker":
    cert = tmp_path / "c.crt"
    key = tmp_path / "c.key"
    cert.write_text("x")
    key.write_text("x")
    w = ws._WalletWorker()
    w.set_params({"host": "localhost", "port": 9256, "fingerprint": 1,
                  "cert_path": str(cert), "key_path": str(key)})
    return w


def test_a_wallet_slower_than_the_old_5s_limit_still_reaches_the_table(tmp_path, monkeypatch):
    stub, calls = _stub_requests(slow_wallet_seconds=6.9)
    monkeypatch.setitem(sys.modules, "requests", stub)
    result = _worker(tmp_path)._fetch_impl()
    assert "CAT db1a9020" in result, "the slow DBX wallet was dropped"
    assert result["CAT db1a9020"]["confirmed"] == pytest.approx(12_411.968)
    assert result["CAT db1a9020"]["asset_id"].startswith("db1a9020")
    balance_timeouts = {t for e, _, t in calls if e == "get_wallet_balance"}
    assert balance_timeouts == {ws._BALANCE_TIMEOUT_S}
    assert ws._BALANCE_TIMEOUT_S >= 2 * 6.9
    # The quick calls keep their short timeout.
    assert {t for e, _, t in calls if e in ("log_in", "get_sync_status", "get_wallets")} == {
        ws._RPC_TIMEOUT_S}


def test_a_failing_wallet_is_logged_once_and_its_recovery_once(tmp_path, monkeypatch, caplog):
    worker = _worker(tmp_path)
    caplog.set_level(logging.DEBUG, logger=ws.__name__)

    stub, _ = _stub_requests(slow_wallet_seconds=0.1, fail_wallets={8})
    monkeypatch.setitem(sys.modules, "requests", stub)
    for _ in range(3):
        result = worker._fetch_impl()
        assert "CAT db1a9020" not in result
        assert "Chia Wallet" in result
    infos = [r for r in caplog.records
             if r.levelno == logging.INFO and "failed" in r.getMessage()]
    assert len(infos) == 1, [r.getMessage() for r in infos]
    assert "wallet 8" in infos[0].getMessage()

    stub, _ = _stub_requests(slow_wallet_seconds=0.1)
    monkeypatch.setitem(sys.modules, "requests", stub)
    assert "CAT db1a9020" in worker._fetch_impl()
    assert "CAT db1a9020" in worker._fetch_impl()
    recovered = [r for r in caplog.records if "works again" in r.getMessage()]
    assert len(recovered) == 1
