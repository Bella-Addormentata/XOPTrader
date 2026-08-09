"""Regression tests for the third PR-73 review round: transport-vs-revert
skip-list poisoning, the persisted relay ledger, the secrets read-modify-write
lock, and unique atomic-write temp names."""

from __future__ import annotations

import threading
from types import SimpleNamespace

import pytest

pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")

from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import evm as evm_mod  # noqa: E402
from gui.services.warp import relayer as relayer_mod  # noqa: E402
from gui.services.warp import service as S  # noqa: E402
from gui.services.warp.nostr import EcdsaSigResult  # noqa: E402

NET = C.MAINNET


# --------------------------------------------------------------------------- #
# #1/#5 the skip-list is poisoned only by deterministic reverts, never by a
# transport outage or an unfunded/underpriced relay key.
# --------------------------------------------------------------------------- #

def test_infrastructure_classifier_splits_transport_from_reverts():
    # Transport / junk: plain EvmError, never message-specific.
    assert evm_mod.is_infrastructure_error(evm_mod.EvmError("connection refused"))
    # Operator / node conditions carried as an RPC error object.
    for msg in ("insufficient funds for gas * price + value",
                "replacement transaction underpriced",
                "nonce too low", "max fee per gas less than block base fee"):
        assert evm_mod.is_infrastructure_error(evm_mod.EvmRpcError(msg)), msg
    # A genuine deterministic revert is NOT infrastructure.
    assert not evm_mod.is_infrastructure_error(
        evm_mod.EvmRpcError("execution reverted: bad signature")
    )
    assert not evm_mod.is_infrastructure_error(
        evm_mod.EvmRpcError("execution reverted: !nonce")
    )


class _Collector:
    def collect_ecdsa(self, *, nonce, digest, threshold, have=None,
                      deadline_s=20.0, relay_offset=0):
        collected = {
            "0x" + f"{i:040x}": bytes([27]) + bytes([i]) * 64
            for i in range(1, threshold + 1)
        }
        return EcdsaSigResult(collected=collected, threshold=threshold)


class _Evm:
    """Enough of EvmClient for the relayer sweep, with a scriptable preflight
    and broadcast error."""

    def __init__(self, *, preflight_error=None, broadcast_error=None) -> None:
        self.preflight_error = preflight_error
        self.broadcast_error = broadcast_error
        self.sent: list = []

    def verify_eip712_domain(self):
        return bytes.fromhex(NET.eip712_domain_separator)

    def get_signature_threshold(self):
        return 6

    def _eth_call(self, to, data):
        if self.preflight_error is not None:
            raise self.preflight_error
        return b""

    def prepare_relay(self, *, owner, calldata, nonce=None, fees=None):
        return SimpleNamespace(kind="relay", data=calldata, nonce=1,
                               gas=150_000, max_fee_per_gas=1_000_000,
                               max_priority_fee_per_gas=100_000)

    def send_raw_transaction(self, raw):
        if self.broadcast_error is not None:
            raise self.broadcast_error
        self.sent.append(bytes(raw))
        return "0x" + "cd" * 32


def _watcher_msg():
    return {
        "nonce": "cd" * 32,
        "source": NET.burn_puzzle_hash,
        "destination": "ab" * 20,
        "contents": ["11" * 32, "22" * 32, "0000001388".rjust(64, "0")],
        "status": "sent",
        "destination_transaction_hash": None,
        "source_timestamp": 1_786_000_000.0 - 7200,
    }


def _relayer(ev, **over):
    return relayer_mod.AltruisticRelayer(
        net=NET, evm_client=ev, collector=_Collector(),
        watcher_fetch=lambda path: [_watcher_msg()],
        evm_key=SimpleNamespace(address="0x" + "ab" * 20, private_key=b"\x11" * 32),
        clock=lambda: 1_786_000_000.0, **over,
    )


def test_transport_preflight_outage_never_skiplists(monkeypatch):
    ev = _Evm(preflight_error=evm_mod.EvmError("RPC timeout"))
    r = _relayer(ev)
    for _ in range(relayer_mod.SKIP_AFTER_FAILURES + 2):
        report = r.sweep()
        assert "transient" in report[0]["action"]
    assert r._skiplist == set(), "a valid message must never be skip-listed on outage"
    assert r._failures == {}


def test_unfunded_relay_key_broadcast_never_skiplists(monkeypatch):
    ev = _Evm(broadcast_error=evm_mod.EvmRpcError(
        "insufficient funds for gas * price + value"))
    r = _relayer(ev)
    monkeypatch.setattr(
        evm_mod, "sign_tx",
        lambda u, k: SimpleNamespace(raw=b"\x02\x01", tx_hash="0x" + "ee" * 32,
                                     nonce=1),
    )
    for _ in range(relayer_mod.SKIP_AFTER_FAILURES + 2):
        report = r.sweep()
        assert "transient" in report[0]["action"]
    assert r._skiplist == set()
    assert ev.sent == []


def test_a_real_revert_still_poisons(monkeypatch):
    ev = _Evm(preflight_error=evm_mod.EvmRpcError("execution reverted: bad sig"))
    r = _relayer(ev)
    for _ in range(relayer_mod.SKIP_AFTER_FAILURES):
        r.sweep()
    assert "cd" * 32 in r._skiplist, "a deterministic revert must skip-list"


# --------------------------------------------------------------------------- #
# #2 the daily ledger and skip-list survive a process restart.
# --------------------------------------------------------------------------- #

def test_relay_ledger_persists_across_a_fresh_worker(tmp_path):
    cfg = {"warp": {"jobs_db": str(tmp_path / "warp_jobs.db")}}

    w1 = S._WarpWorker()
    w1.set_config(cfg)
    w1._relay_state = {"budget": {"day": 20_671, "spent": 123_456},
                       "failures": {"aa": 2}, "skiplist": {"deadbeef"}}
    w1._save_relay_state()

    # A brand-new worker (a relaunched process) must resume the same ledger.
    w2 = S._WarpWorker()
    w2.set_config(cfg)
    w2._load_relay_state()
    assert w2._relay_state["budget"] == {"day": 20_671, "spent": 123_456}
    assert w2._relay_state["failures"] == {"aa": 2}
    assert w2._relay_state["skiplist"] == {"deadbeef"}, "skiplist restored as a set"


def test_a_missing_or_corrupt_ledger_starts_empty(tmp_path):
    cfg = {"warp": {"jobs_db": str(tmp_path / "warp_jobs.db")}}
    w = S._WarpWorker()
    w.set_config(cfg)
    w._load_relay_state()               # no file yet
    assert w._relay_state == {}
    w._relay_state_path().write_text("}{ not json", encoding="utf-8")
    w._load_relay_state()               # corrupt -> logged, left as-is, no raise
    assert isinstance(w._relay_state, dict)


# --------------------------------------------------------------------------- #
# #3 the secrets read-modify-write is serialized against a concurrent write.
# --------------------------------------------------------------------------- #

def test_file_transaction_serializes_concurrent_writers(tmp_path):
    from gui.services import config_split as cs

    p = tmp_path / "secrets.yaml"
    order: list = []
    started = threading.Event()

    def slow_writer():
        with cs.file_transaction(p):
            order.append("A-enter")
            started.set()
            # Hold the lock; B must not interleave.
            import time as _t
            _t.sleep(0.05)
            order.append("A-exit")

    t = threading.Thread(target=slow_writer)
    t.start()
    started.wait(1.0)
    with cs.file_transaction(p):
        order.append("B-enter")
    t.join(2.0)
    assert order == ["A-enter", "A-exit", "B-enter"], \
        "B entered only after A released the same-path lock"


def test_the_lock_is_shared_between_settings_and_wallet_io(tmp_path):
    from gui.services import config_split as cs

    p = tmp_path / "secrets.yaml"
    io = S._SecretsFileIO(p)
    # Same resolved path -> the very same lock object.
    assert cs.file_lock(p) is cs.file_lock(p.resolve())

    # While the wallet IO holds its transaction, ANOTHER thread trying to take
    # the settings-side lock for the same path must block -- proving the two
    # write paths are serialized against each other.
    held = threading.Event()
    other_acquired = threading.Event()

    def other():
        got = cs.file_lock(p).acquire(timeout=0.2)
        if got:
            other_acquired.set()
            cs.file_lock(p).release()

    with io.transaction():
        t = threading.Thread(target=other)
        t.start()
        held.set()
        t.join(1.0)
        assert not other_acquired.is_set(), \
            "a concurrent settings-side acquire must block on the shared lock"
    # Once released, the same lock is acquirable again.
    assert cs.file_lock(p).acquire(timeout=1.0)
    cs.file_lock(p).release()


# --------------------------------------------------------------------------- #
# #4 concurrent atomic writes use unique temp files (no collision).
# --------------------------------------------------------------------------- #

def test_atomic_writes_use_unique_temps_under_thread_races(tmp_path):
    from gui.services import config_split as cs

    p = tmp_path / "secrets.yaml"
    errors: list = []

    def writer(tag):
        try:
            for _ in range(20):
                cs._atomic_write_text(p, f"warp:\n  who: {tag}\n", newline="\n")
        except Exception as exc:  # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=writer, args=(t,)) for t in ("A", "B", "C")]
    for t in threads:
        t.start()
    for t in threads:
        t.join(5.0)
    assert errors == [], f"no writer collided on a temp file: {errors}"
    # The file is one writer's WHOLE content (never a mix), and no temp residue.
    body = p.read_text(encoding="utf-8")
    assert body.strip() in ("warp:\n  who: A", "warp:\n  who: B", "warp:\n  who: C")
    assert not any(f.name.startswith(".secrets.yaml.") for f in tmp_path.iterdir())
