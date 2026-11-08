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


# [review] Shutdown.  quit() cannot interrupt a blocking RPC, and a QThread
# destroyed while still running aborts the process (pinned in
# test_worker_thread_shutdown.py).  With 15 s balance reads, stop()'s old
# 5 s wait could return with a read still in flight, so a GUI close during
# the slow DBX read could crash.  stop() now asks the worker to abandon the
# pass between RPCs and waits out the one RPC in flight.


def test_a_stop_during_a_balance_read_ends_the_pass_after_that_read(tmp_path, monkeypatch):
    worker = _worker(tmp_path)
    stub, calls = _stub_requests(slow_wallet_seconds=0.1)
    inner_post = stub.post
    stop_once = [True]

    def post(url, **kw):
        resp = inner_post(url, **kw)
        if (stop_once and url.endswith("/get_wallet_balance")
                and kw["json"]["wallet_id"] == 1):
            stop_once.clear()
            worker.request_abort()  # stop() arrives while wallet 1 is read
        return resp

    stub.post = post
    monkeypatch.setitem(sys.modules, "requests", stub)
    assert worker._fetch_impl() == {}
    reads = [j["wallet_id"] for e, j, _ in calls if e == "get_wallet_balance"]
    assert reads == [1], "the pass kept reading wallets after stop()"

    # A pass that starts after stop() makes no RPC at all.
    calls.clear()
    assert worker._fetch_impl() == {}
    assert calls == []

    # start() clears the request, so a restarted service reads again.
    worker.clear_abort()
    assert "CAT db1a9020" in worker._fetch_impl()


def test_stop_joins_a_worker_that_is_mid_read(tmp_path, monkeypatch):
    import threading
    import time

    from PySide6.QtCore import QCoreApplication

    app = QCoreApplication.instance() or QCoreApplication([])  # noqa: F841
    assert ws._STOP_WAIT_MS >= ws._BALANCE_TIMEOUT_S * 1000

    stub, calls = _stub_requests(slow_wallet_seconds=0.1)
    inner_post = stub.post
    reading = threading.Event()

    def post(url, **kw):
        if url.endswith("/get_wallet_balance") and kw["json"]["wallet_id"] == 1:
            reading.set()
            time.sleep(1.0)  # a slow read the GUI closes in the middle of
        return inner_post(url, **kw)

    stub.post = post
    monkeypatch.setitem(sys.modules, "requests", stub)

    cert = tmp_path / "c.crt"
    key = tmp_path / "c.key"
    cert.write_text("x")
    key.write_text("x")
    svc = ws.WalletService({"chia": {"wallet_fingerprint": 1,
                                     "wallet_cert_path": str(cert),
                                     "wallet_key_path": str(key)}})
    svc.start()
    svc.fetch_balances()
    assert reading.wait(10.0), "the worker never started its pass"
    svc.stop()
    assert not svc._thread.isRunning(), "stop() returned with the read in flight"
    reads = [j["wallet_id"] for e, j, _ in calls if e == "get_wallet_balance"]
    assert reads == [1]
