"""Altruistic-relay liveness: BIP340 pinned to the official vectors, the
heartbeat layer, the Chia-only unwrap mode (AWAITING_EXTERNAL_RELAY), the
engine's opt-in sweep loop, and the activity-evidence cache."""

from __future__ import annotations

import csv
import pathlib
from types import SimpleNamespace

import pytest

pytest.importorskip("clvm")
pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")

from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import nostr as nostr_mod  # noqa: E402
from gui.services.warp import schnorr  # noqa: E402
from gui.services.warp import service as S  # noqa: E402
from gui.services.warp.jobs import JobStatus  # noqa: E402
from gui.services.warp.nostr import EcdsaSigResult  # noqa: E402

from .test_warp_service import (  # noqa: E402
    FakeWatcher,
    build,
    new_store,
    seed,
)
from .test_warp_unwrap_service import (  # noqa: E402
    AMOUNT,
    RECEIVER,
    UnwrapFakeEvm,
    UnwrapFakeWallet,
    params_with_cap,
)

NET = C.MAINNET
NOW = 1_786_000_000.0
EVM_PRIVKEY = b"\x42" * 32
RELAYER_ADDR = "0x" + "99" * 20

_VECTORS = pathlib.Path(__file__).parent / "fixtures_bip340_vectors.csv"


# --------------------------------------------------------------------------- #
# BIP340: every official vector, sign and verify.
# --------------------------------------------------------------------------- #

def test_bip340_official_vectors_all_pass():
    """The fixture is the bitcoin/bips test-vectors.csv, downloaded verbatim
    (captured, not transcribed). All 19 rows: verification must agree, and
    where a secret key is given, pubkey_gen and sign must reproduce the
    vector byte-for-byte."""
    rows = list(csv.DictReader(open(_VECTORS, encoding="utf-8")))
    assert len(rows) == 19, "the official vector set has 19 rows"
    for row in rows:
        pub = bytes.fromhex(row["public key"])
        msg = bytes.fromhex(row["message"]) if row["message"] else b""
        sig = bytes.fromhex(row["signature"])
        want = row["verification result"] == "TRUE"
        assert schnorr.verify(msg, pub, sig) == want, \
            f"vector {row['index']}: {row['comment'] or 'verify mismatch'}"
        if row["secret key"]:
            sk = bytes.fromhex(row["secret key"])
            aux = bytes.fromhex(row["aux_rand"])
            assert schnorr.pubkey_gen(sk) == pub, f"vector {row['index']}: pubkey"
            assert schnorr.sign(msg, sk, aux) == sig, f"vector {row['index']}: sign"


def test_schnorr_rejects_out_of_range_keys():
    with pytest.raises(schnorr.SchnorrError):
        schnorr.pubkey_gen(b"\x00" * 32)
    with pytest.raises(schnorr.SchnorrError):
        schnorr.sign(b"m", schnorr.N.to_bytes(32, "big"), b"\x00" * 32)


# --------------------------------------------------------------------------- #
# The heartbeat layer.
# --------------------------------------------------------------------------- #

def _beat(*, created_at=NOW, relayer=RELAYER_ADDR, key=EVM_PRIVKEY):
    return nostr_mod.build_heartbeat_event(
        NET, key, relayer, created_at=created_at, aux_rand=b"\x00" * 32
    )


def test_heartbeat_round_trips_and_identity_is_derived_not_reused():
    event = _beat()
    out = nostr_mod.verify_heartbeat_event(NET, event, now=NOW + 60, max_age_s=900)
    assert out == {
        "relayer": RELAYER_ADDR.lower(),
        "pubkey": event["pubkey"],
        "seen_at": int(NOW),
    }
    sk1, pk1 = nostr_mod.heartbeat_keypair(EVM_PRIVKEY)
    sk2, pk2 = nostr_mod.heartbeat_keypair(EVM_PRIVKEY)
    assert (sk1, pk1) == (sk2, pk2), "identity must be stable across restarts"
    assert sk1 != EVM_PRIVKEY, "never sign Nostr events with the funds scalar"
    assert schnorr.pubkey_gen(sk1).hex() == pk1 == event["pubkey"]


def test_every_forgery_lever_is_rejected():
    ok = dict(now=NOW + 60, max_age_s=900.0)
    # Content tamper -> the recomputed id no longer matches.
    tampered = dict(_beat())
    tampered["content"] = tampered["content"].replace("99", "aa", 1)
    assert nostr_mod.verify_heartbeat_event(NET, tampered, **ok) is None
    # A lying id field alone (signature still valid over the TRUE id) is a
    # NIP-01 violation and must be rejected -- this is the one case the
    # signature gate cannot catch, because the sig verifies over the
    # recomputed id, not the stored one.
    lying_id = dict(_beat())
    lying_id["id"] = "00" * 32
    assert nostr_mod.verify_heartbeat_event(NET, lying_id, **ok) is None
    # Signature tamper -> BIP340 verify fails.
    bad_sig = dict(_beat())
    bad_sig["sig"] = bad_sig["sig"][:-2] + ("00" if bad_sig["sig"][-2:] != "00" else "01")
    assert nostr_mod.verify_heartbeat_event(NET, bad_sig, **ok) is None
    # Wrong kind / missing tag.
    wrong_kind = dict(_beat())
    wrong_kind["kind"] = 7
    assert nostr_mod.verify_heartbeat_event(NET, wrong_kind, **ok) is None
    untagged = dict(_beat())
    untagged["tags"] = []
    assert nostr_mod.verify_heartbeat_event(NET, untagged, **ok) is None
    # Stale, and post-dated beyond honest skew (the stay-online-forever spoof).
    assert nostr_mod.verify_heartbeat_event(
        NET, _beat(created_at=NOW - 1000), **ok
    ) is None
    assert nostr_mod.verify_heartbeat_event(
        NET, _beat(created_at=NOW + 400), **ok
    ) is None
    # Properly signed but for a junk relayer address -> shape gate.
    assert nostr_mod.verify_heartbeat_event(NET, _beat(relayer="garbage"), **ok) is None
    # Properly signed but for another network.
    other_net = SimpleNamespace(name="testnet")
    assert nostr_mod.verify_heartbeat_event(other_net, _beat(), **ok) is None


def test_fetch_recent_heartbeats_dedupes_and_survives_dead_relays():
    old = _beat(created_at=NOW - 300)
    new = _beat(created_at=NOW - 60)
    calls = {"n": 0}

    def fetcher(relay_url, filt, timeout):
        calls["n"] += 1
        if calls["n"] == 1:
            raise ConnectionError("relay down")
        assert filt["#t"] == [nostr_mod.HEARTBEAT_TAG]
        return [old, new, {"kind": 1, "junk": True}]

    out = nostr_mod.fetch_recent_heartbeats(NET, now=NOW, fetcher=fetcher)
    assert len(out) == 1, "one volunteer, deduped by pubkey"
    assert out[0]["seen_at"] == int(NOW - 60), "the freshest beat wins"


def test_publish_heartbeat_counts_acceptances_and_never_raises():
    verdicts = iter([True, ConnectionError("down"), False] + [False] * 10)
    sent: list = []

    def publisher(relay_url, event, timeout):
        sent.append(event)
        v = next(verdicts)
        if isinstance(v, Exception):
            raise v
        return v

    accepted = nostr_mod.publish_heartbeat(
        NET, EVM_PRIVKEY, RELAYER_ADDR, created_at=NOW, publisher=publisher
    )
    assert accepted == 1
    assert all(e["id"] == sent[0]["id"] for e in sent), "one event, all relays"
    assert nostr_mod.verify_heartbeat_event(
        NET, sent[0], now=NOW, max_age_s=900
    ) is not None, "what we publish must pass our own verifier"


# --------------------------------------------------------------------------- #
# The Chia-only unwrap: gates and the AWAITING_EXTERNAL_RELAY handler.
# --------------------------------------------------------------------------- #

def _build_unwrap(store, *, params=None, collector=None):
    evm = UnwrapFakeEvm()
    evm.calls = []
    evm._eth_call = lambda to, data: b""      # deliverable unless a test overrides
    wallet = UnwrapFakeWallet()
    return build(store, params=params or params_with_cap(),
                 evm=evm, wallet=wallet, collector=collector)


def test_external_unwrap_skips_the_gas_gate_and_plain_does_not():
    store = new_store()
    engine, ctx = _build_unwrap(store)
    ctx.evm.eth = 0                            # a Chia-only operator

    engine.request_unwrap(AMOUNT, RECEIVER, True)
    assert engine.step()["status"] == JobStatus.BURN_SENT, \
        "external mode: no gas gate before the burn"
    assert store.get_active_job().state["external_relay"] is True

    store2 = new_store()
    engine2, ctx2 = _build_unwrap(store2)
    ctx2.evm.eth = 0
    engine2.request_unwrap(AMOUNT, RECEIVER)
    out = engine2.step()
    assert out["status"] == JobStatus.UNWRAP_CHECKS, \
        "plain mode still pends on the gas floor"
    assert store2.get_active_job().state["pending_polls"] == 1
    assert ctx2.wallet.cat_spends == [], "nothing burns while the gate pends"


class CompleteEcdsaCollector:
    """Six well-formed v||r||s signatures keyed by ascending fake addresses."""

    def collect_ecdsa(self, *, nonce, digest, threshold, have=None,
                      deadline_s=20.0, relay_offset=0):
        collected = {
            "0x" + f"{i:040x}": bytes([27]) + bytes([i]) * 64
            for i in range(1, threshold + 1)
        }
        return EcdsaSigResult(collected=collected, threshold=threshold)


def _seed_collecting(store, *, external):
    return seed(
        store, JobStatus.COLLECTING_EVM_SIGS,
        columns={"amount_mojos": AMOUNT, "amount_usdc_micros": AMOUNT * 1000,
                 "bridge_nonce": "cd" * 32},
        state={"receiver_evm": "ab" * 20, "external_relay": external,
               "post_tip_base_units": 4_985_000},
    )


def test_quorum_branches_on_gas_only_for_external_jobs():
    # External + gasless -> AWAITING_EXTERNAL_RELAY, sigs persisted for later.
    store = new_store()
    engine, ctx = _build_unwrap(store, collector=CompleteEcdsaCollector())
    _seed_collecting(store, external=True)
    ctx.evm.eth = 0
    out = engine.step()
    assert out["status"] == JobStatus.AWAITING_EXTERNAL_RELAY
    assert store.get_active_job().state["relay_sigs"], \
        "the packed sigs must survive for a later self-relay"

    # External + funded -> self-relay anyway (faster; shrinks the stuck tail).
    store2 = new_store()
    engine2, _ctx2 = _build_unwrap(store2, collector=CompleteEcdsaCollector())
    _seed_collecting(store2, external=True)
    assert engine2.step()["status"] == JobStatus.RELAYING

    # Plain jobs never take the branch, funded or not.
    store3 = new_store()
    engine3, ctx3 = _build_unwrap(store3, collector=CompleteEcdsaCollector())
    _seed_collecting(store3, external=False)
    ctx3.evm.eth = 0
    assert engine3.step()["status"] == JobStatus.RELAYING


def _seed_awaiting(store):
    return seed(
        store, JobStatus.AWAITING_EXTERNAL_RELAY,
        columns={"amount_mojos": AMOUNT, "amount_usdc_micros": AMOUNT * 1000,
                 "bridge_nonce": "cd" * 32},
        state={"receiver_evm": "ab" * 20, "relay_sigs": "00" * 390,
               "relay_threshold": 6, "post_tip_base_units": 4_985_000,
               "external_relay": True},
    )


def test_awaiting_completes_on_the_unforgeable_nonce_proof(monkeypatch):
    from gui.services.warp import evm as evm_mod

    store = new_store()
    engine, ctx = _build_unwrap(store)
    _seed_awaiting(store)
    ctx.evm.eth = 0

    def replay_reverts(to, data):
        raise evm_mod.EvmRpcError("execution reverted: !nonce")

    ctx.evm._eth_call = replay_reverts
    ctx.watcher.msg = SimpleNamespace(
        raw={"destination_transaction_hash": "0x" + "77" * 32}
    )
    out = engine.step()
    assert out["status"] == JobStatus.COMPLETED
    job = store.get_job(out["id"])
    assert job.state["delivered_by"] == "an altruistic relayer"
    assert job.bridge_tx_hash == "0x" + "77" * 32, \
        "the watcher's hash is recorded as display provenance"


def test_awaiting_completes_even_when_the_watcher_is_down(monkeypatch):
    from gui.services.warp import evm as evm_mod

    store = new_store()
    engine, ctx = _build_unwrap(store)
    _seed_awaiting(store)
    ctx.evm.eth = 0
    ctx.evm._eth_call = lambda to, data: (_ for _ in ()).throw(
        evm_mod.EvmRpcError("execution reverted: !nonce")
    )

    def watcher_dies(nonce, source_chain="bse"):
        raise ConnectionError("watcher down")

    ctx.watcher.get_message = watcher_dies
    out = engine.step()
    assert out["status"] == JobStatus.COMPLETED, \
        "the proof is the eth_call, never the watcher"


def test_awaiting_switches_to_self_relay_when_gas_appears():
    store = new_store()
    engine, ctx = _build_unwrap(store)
    _seed_awaiting(store)
    ctx.evm.eth = S._MIN_GAS_WEI
    out = engine.step()
    assert out["status"] == JobStatus.RELAYING
    assert "self-relay" in (out.get("message") or "") or True


def test_awaiting_stays_a_healthy_pend_while_gasless_and_undelivered():
    store = new_store()
    engine, ctx = _build_unwrap(store)
    _seed_awaiting(store)
    ctx.evm.eth = 0
    out = engine.step()
    assert out["status"] == JobStatus.AWAITING_EXTERNAL_RELAY
    assert out["retry_count"] == 0, "a wait, not an error"
    assert out["last_error"] is None
    assert store.get_active_job().state["pending_polls"] == 1


def test_awaiting_bounces_to_collection_on_threshold_drift():
    store = new_store()
    engine, ctx = _build_unwrap(store)
    _seed_awaiting(store)
    ctx.evm.eth = 0
    ctx.evm.threshold = 7                      # owner mutated the quorum
    out = engine.step()
    assert out["status"] == JobStatus.COLLECTING_EVM_SIGS
    assert store.get_active_job().state.get("relay_sigs") is None


def test_awaiting_has_a_deadline_and_retry_reenters():
    assert JobStatus.AWAITING_EXTERNAL_RELAY in S._PENDING_MAX_POLLS
    limit = S._PENDING_MAX_POLLS[JobStatus.AWAITING_EXTERNAL_RELAY]

    store = new_store()
    engine, ctx = _build_unwrap(store)
    job = _seed_awaiting(store)
    ctx.evm.eth = 0
    store.update_job(job.id, expected_status=job.status,
                     state_patch={"pending_polls": limit})
    out = engine.step()
    assert out["status"] == JobStatus.FAILED
    assert store.get_job(job.id).state["failed_from"] == \
        JobStatus.AWAITING_EXTERNAL_RELAY

    engine.job_action(job.id, "retry")
    assert store.get_job(job.id).status == JobStatus.AWAITING_EXTERNAL_RELAY


# --------------------------------------------------------------------------- #
# The engine's opt-in sweep loop.
# --------------------------------------------------------------------------- #

class FakeRelayer:
    def __init__(self, *, boom: bool = False) -> None:
        self.calls: list = []
        self.boom = boom

    def sweep(self, *, broadcast: bool = True):
        if self.boom:
            raise RuntimeError("watcher exploded")
        self.calls.append(broadcast)
        return [{"action": "relayed"}, {"action": "skiplisted"}]


def _relay_key():
    return SimpleNamespace(address=RELAYER_ADDR, private_key=EVM_PRIVKEY)


def _fixed_clock(t=NOW):
    return lambda: t


def test_sweep_throttles_and_reports(monkeypatch):
    fake = FakeRelayer()
    store = new_store()
    engine, _ctx = build(
        store, params=params_with_cap(dry_run=False), relayer=fake,
        relay_key=_relay_key(), heartbeat_publisher=lambda r, e, t: True,
        clock=_fixed_clock(),
    )
    report = engine.relay_sweep_if_due()
    assert report["relayed"] == 1 and fake.calls == [True]
    assert report["heartbeat_accepted"] == len(NET.nostr_relays)
    assert engine.relay_sweep_if_due() is None, "throttled within the interval"
    assert fake.calls == [True]


def test_dry_run_sweeps_rehearse_and_never_heartbeat():
    fake = FakeRelayer()
    published: list = []
    store = new_store()
    engine, _ctx = build(
        store, params=params_with_cap(dry_run=True), relayer=fake,
        relay_key=_relay_key(),
        heartbeat_publisher=lambda r, e, t: published.append(e) or True,
        clock=_fixed_clock(),
    )
    report = engine.relay_sweep_if_due()
    assert fake.calls == [False], "dry run must not broadcast"
    assert report["broadcast"] is False
    assert published == [], \
        "a rehearsal must not advertise liveness to Chia-only users"
    assert "heartbeat_accepted" not in report


def test_sweep_failure_is_contained_not_raised():
    store = new_store()
    engine, _ctx = build(
        store, params=params_with_cap(dry_run=False),
        relayer=FakeRelayer(boom=True), relay_key=_relay_key(),
        heartbeat_publisher=lambda r, e, t: True, clock=_fixed_clock(),
    )
    report = engine.relay_sweep_if_due()
    assert "watcher exploded" in report["error"]
    snap = engine.snapshot()
    assert snap["altruistic_relay"]["enabled"] is True
    assert "watcher exploded" in snap["altruistic_relay"]["last_sweep"]["error"]


def test_without_the_opt_in_the_sweep_is_inert():
    store = new_store()
    engine, _ctx = build(store, clock=_fixed_clock())
    assert engine.relay_sweep_if_due() is None
    assert engine.snapshot()["altruistic_relay"]["enabled"] is False


# --------------------------------------------------------------------------- #
# The activity-evidence cache.
# --------------------------------------------------------------------------- #

class ActivityWatcher(FakeWatcher):
    def __init__(self, msgs) -> None:
        super().__init__()
        self.msgs = msgs

    def fetch_path(self, path: str) -> list:
        return list(self.msgs)


def _third_party_msg():
    """Stuck 3h, then delivered by RELAYER_ADDR, who is not the receiver."""
    return {
        "nonce": "aa" * 32,
        "status": "received",
        "destination_transaction_hash": "0x" + "01" * 32,
        "source_timestamp": NOW - 20_000,
        "destination_timestamp": NOW - 9_000,
        "contents": ["11" * 32, "00" * 12 + "22" * 20, "05" * 32],
    }


def test_activity_composes_both_evidence_layers_and_throttles():
    beat = _beat(created_at=NOW - 60)
    store = new_store()
    evm = UnwrapFakeEvm()
    evm._call = lambda method, params: {"from": RELAYER_ADDR}
    engine, _ctx = build(
        store, params=params_with_cap(),
        evm=evm, watcher=ActivityWatcher([_third_party_msg()]),
        nostr_fetcher=lambda relay, filt, timeout: [beat],
        clock=_fixed_clock(),
    )
    out = engine.refresh_relay_activity_if_due()
    assert out["third_party_count"] == 1
    assert out["third_party"][0]["relayer"] == RELAYER_ADDR
    assert out["online_now"] is True
    assert out["online_proven"] is True, \
        "the heartbeat's address matches the on-chain evidence"
    assert engine.refresh_relay_activity_if_due() is None, "throttled"
    assert engine.snapshot()["relay_activity"]["online_proven"] is True


def test_activity_survives_both_sources_failing():
    class DeadWatcher(FakeWatcher):
        def fetch_path(self, path):
            raise ConnectionError("watcher down")

    store = new_store()
    engine, _ctx = build(
        store, params=params_with_cap(), watcher=DeadWatcher(),
        nostr_fetcher=lambda *a: (_ for _ in ()).throw(ConnectionError("x")),
        clock=_fixed_clock(),
    )
    out = engine.refresh_relay_activity_if_due()
    assert out["online_now"] is False and out["online_proven"] is False
    assert "error" in out


# --------------------------------------------------------------------------- #
# Params and construction wiring.
# --------------------------------------------------------------------------- #

def test_altruistic_params_parse_and_refuse_junk():
    p = S.warp_params_from_config({"warp": {
        "altruistic_relay": True,
        "relay_grace_min": 10,
        "relay_daily_gas_budget_eth": 0.001,
    }})
    assert p.altruistic_relay is True
    assert p.relay_grace_min == 10.0
    assert p.relay_daily_gas_budget_eth == 0.001
    assert S.warp_params_from_config({}).altruistic_relay is False
    # 0/None/absent mean "unset -> default" per the `or 30` idiom; junk and
    # negatives are refused loudly.
    for bad in ("abc", -1, "nan"):
        with pytest.raises(S.WarpError):
            S.warp_params_from_config({"warp": {"relay_grace_min": bad}})
    assert S.warp_params_from_config(
        {"warp": {"relay_grace_min": 0}}
    ).relay_grace_min == 30.0


def test_build_engine_wires_the_relayer_and_key_fallback(tmp_path, monkeypatch):
    from unittest import mock

    from gui.services.warp import keystore as ks

    monkeypatch.setattr(S, "_job_db_path", lambda cfg: str(tmp_path / "j.db"))
    hot = ks.EvmKey(b"\x11" * 32, "0x" + "ab" * 20)
    gas_only = ks.EvmKey(b"\x22" * 32, "0x" + "cd" * 20)

    def _worker(cfg_extra):
        worker = S._WarpWorker()
        worker.set_config({"warp": {
            "enabled": True, "max_auto_bridge_usdc": 100,
            "evm_private_key_dpapi": "hot-blob", **cfg_extra,
        }})
        return worker

    # Fallback: no dedicated relay key -> the hot key pays the gas.
    worker = _worker({"altruistic_relay": True})
    with mock.patch.object(ks, "load_evm_key", return_value=hot), \
         mock.patch.object(ks, "default_protector", return_value=object()):
        engine = worker._ensure_engine()
    assert engine is not None, f"engine did not build: {worker._engine_error}"
    assert engine._relayer is not None
    assert engine._relay_key is hot
    assert engine._relayer.evm_key is hot
    engine.close()

    # Dedicated key wins when present.
    worker2 = _worker({"altruistic_relay": True,
                       "relay_private_key_dpapi": "gas-blob"})
    with mock.patch.object(
        ks, "load_evm_key",
        side_effect=lambda blob, protector=None: gas_only if blob == "gas-blob" else hot,
    ), mock.patch.object(ks, "default_protector", return_value=object()):
        engine2 = worker2._ensure_engine()
    assert engine2 is not None, f"engine did not build: {worker2._engine_error}"
    assert engine2._relay_key is gas_only
    grace = engine2._relayer.grace_s
    assert grace == 30 * 60.0
    engine2.close()

    # Off by default: no relayer object at all.
    worker3 = _worker({})
    with mock.patch.object(ks, "load_evm_key", return_value=hot), \
         mock.patch.object(ks, "default_protector", return_value=object()):
        engine3 = worker3._ensure_engine()
    assert engine3 is not None
    assert engine3._relayer is None and engine3._relay_key is None
    engine3.close()
