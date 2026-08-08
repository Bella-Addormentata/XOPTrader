"""Crash-resume tests for the warp bridge state machine (:mod:`service`).

These drive :class:`WarpEngine.step` through every state with fully injected
fakes and zero I/O, asserting the persist-then-act invariants that make a GUI
hard-kill safe:

* deposit freezes the amounts/receiver and never re-derives them later;
* EVM broadcasts are two-phase -- signed once, raw persisted, re-broadcast
  idempotently -- so a crash re-broadcasts the *same* tx (no double-sign);
* claim funding dedupe-scans before ``send_transaction`` (no double-fund);
* the attested-terms anchors fail *closed* (mismatch -> FAILED, funds frozen);
* portal advancement wipes stale signatures rather than reusing them;
* the dispatcher's error taxonomy (pending -> stay, transient -> backoff,
  terminal -> FAILED) and the backoff gate behave exactly as designed.

The engine is pure, so no Qt is imported. ``chia_rs``/``clvm`` are needed for the
real ``keystore``/``clvm_utils``/``drivers`` paths the engine still exercises
(ephemeral BLS keygen, xch address round-trip, terminal classification), so the
whole module ``importorskip``s them, mirroring ``test_warp_nostr.py``. Everything
chain-touching (``claim.*``) and the EVM signer (``evm.sign_tx``) is monkeypatched;
the pure EVM math (``bridgeable_mojo``/``post_tip_amount``/``parse_message_sent_nonce``/
``receipt_reverted``) and ``nostr.sig_switches_for`` run for real.
"""

from __future__ import annotations

from types import SimpleNamespace

import pytest

pytest.importorskip("clvm")  # clvm_utils / drivers import clvm
pytest.importorskip("chia_rs")  # keystore.new_bls_key + drivers need BLS

from gui.services.warp import claim as claim_mod  # noqa: E402
from gui.services.warp import clvm_utils as cu  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import evm as evm_mod  # noqa: E402
from gui.services.warp import keystore  # noqa: E402
from gui.services.warp import nostr  # noqa: E402
from gui.services.warp import service as S  # noqa: E402
from gui.services.warp.jobs import JobStatus, WarpJobStore  # noqa: E402

NET = C.MAINNET
RECEIVER_PH = bytes(range(32))
ADDR = cu.encode_puzzle_hash(RECEIVER_PH, NET.chia_prefix)


def _hx_pad32(value: str) -> str:
    """Bare hex left-padded to a 32-byte word, as warp message contents are."""
    s = str(value or "")
    if s[:2].lower() == "0x":
        s = s[2:]
    return s.lower().rjust(64, "0")


# --------------------------------------------------------------------------- #
# Fakes.
# --------------------------------------------------------------------------- #

class FakeProtector:
    """Reversible identity 'encryption' that is still entropy-sensitive.

    Prepends the entropy so an ``unprotect`` under different entropy fails,
    proving the per-job entropy binding round-trips, without needing DPAPI.
    """

    def protect(self, data: bytes, entropy: bytes = b"") -> bytes:
        return bytes(entropy) + b"|" + bytes(data)

    def unprotect(self, blob: bytes, entropy: bytes = b"") -> bytes:
        prefix = bytes(entropy) + b"|"
        if not bytes(blob).startswith(prefix):
            raise ValueError("entropy mismatch")
        return bytes(blob)[len(prefix):]


class FakeEvm:
    """Injected Base client. Public attributes let a test vary responses per tick."""

    def __init__(self) -> None:
        self.erc20 = 0
        self.eth = S._MIN_GAS_WEI * 10
        self.tip_bps = 30
        self.allowance = 0
        self.receipt = None
        self.confs = 0
        self.sent_raw: list = []
        self.prepared: list = []
        self.erc20_calls = 0
        self.raise_erc20: Exception | None = None

    def get_erc20_balance(self, token, owner):
        self.erc20_calls += 1
        if self.raise_erc20 is not None:
            raise self.raise_erc20
        return self.erc20

    def get_eth_balance(self, owner):
        return self.eth

    def get_tip_bps(self):
        return self.tip_bps

    def get_bridge_allowance(self, owner):
        return self.allowance

    def prepare_approve(self, *, owner, amount_base_units):
        self.prepared.append(("approve", amount_base_units))
        return {"kind": "approve", "amount": amount_base_units}

    def prepare_bridge(self, *, owner, receiver_ph, mojo_amount):
        self.prepared.append(("bridge", mojo_amount))
        return {"kind": "bridge", "mojo": mojo_amount}

    def send_raw_transaction(self, raw):
        self.sent_raw.append(bytes(raw))
        return "0x" + "cd" * 32

    def get_transaction_receipt(self, tx_hash):
        return self.receipt

    def get_confirmations(self, tx_hash):
        return self.confs


class FakeWallet:
    def __init__(self) -> None:
        self.logged_in = 0
        self.sent: list = []
        self.txs: list = []
        self.next_address = ADDR

    def log_in(self):
        self.logged_in += 1

    def get_next_address(self, wallet_id, new_address=True):
        return self.next_address

    def send_transaction(self, wallet_id, amount, address, fee_mojos=0):
        self.sent.append((amount, address, fee_mojos))
        return {"name": f"funding-tx-{len(self.sent)}"}

    def get_transactions(self, wallet_id, start=0, end=200, reverse=True):
        return list(self.txs)


class FakeCoinset:
    def __init__(self) -> None:
        self.record = None
        self.peak = 0
        self.by_ph: list = []

    def get_coin_record_by_name(self, name):
        return self.record

    def get_peak_height(self):
        return self.peak

    def get_coin_records_by_puzzle_hash(self, ph, include_spent=False):
        return list(self.by_ph)


class FakeWatcher:
    def __init__(self, msg=None) -> None:
        self.msg = msg

    def get_message(self, nonce, source_chain="bse"):
        return self.msg


class FakeCollector:
    def __init__(self, result) -> None:
        self.result = result
        self.calls: list = []

    def collect(self, *, nonce, portal_coin_id, digest, have=None, deadline_s=20.0, relay_offset=0):
        self.calls.append(
            {"have": dict(have or {}), "relay_offset": relay_offset,
             "portal": bytes(portal_coin_id)}
        )
        return self.result


class FakeCoin:
    """A drivers-style coin exposing ``name() -> bytes`` (its coin id)."""

    def __init__(self, coin_id: bytes) -> None:
        self._id = bytes(coin_id)

    def name(self) -> bytes:
        return self._id


def fake_sign_tx(unsigned, key):
    """Deterministic signer: distinct raw per tx kind, hex-round-trippable."""
    kind = unsigned.get("kind") if isinstance(unsigned, dict) else "x"
    if kind == "bridge":
        return SimpleNamespace(raw=b"\x02\xbb\xbb\xbb", tx_hash="0x" + "22" * 32, nonce=3)
    return SimpleNamespace(raw=b"\x02\xaa\xaa\xaa", tx_hash="0x" + "11" * 32, nonce=0)


def message_sent_receipt(nonce_hex: str) -> dict:
    """A non-reverted receipt carrying one MessageSent log from the portal."""
    return {
        "status": "0x1",
        "logs": [
            {"address": NET.portal_address,
             "topics": ["0x" + evm_mod.MESSAGE_SENT_TOPIC0, "0x" + nonce_hex]},
        ],
    }


def big_clock(step: float = 100_000.0):
    """Monotonic epoch clock that jumps far each call.

    A post-stay ``next_retry_at = now + 15`` therefore always lies well below the
    next ``_now()``, so the dispatcher backoff gate never *wrongly* blocks in a
    multi-tick test (the gate is exercised explicitly with a far-future value).
    """
    box = {"t": 0.0}

    def _clock() -> float:
        box["t"] += step
        return box["t"]

    return _clock


# --------------------------------------------------------------------------- #
# Builders.
# --------------------------------------------------------------------------- #

def new_store() -> WarpJobStore:
    return WarpJobStore(":memory:", now=lambda: "2026-01-01T00:00:00+00:00")


def default_params(**over) -> S.WarpParams:
    base = dict(
        enabled=False,
        # Most tests drive the live path; the dry-run cut points get their own
        # dedicated tests that flip this on.
        dry_run=False,
        auto_bridge=False,
        min_micros=1_000_000,
        max_micros=0,
        claim_fee_mojos=100_000_000,
        chia_funding_fee_mojos=0,
        poll_interval_s=15.0,
        chia_receiver_address=ADDR,
        # A static hint so tests never fall through to the Nostr bootstrap (which
        # would touch the network); the value is inert because sync_portal is
        # patched, and jobs that carry their own portal_coin_id override it.
        portal_hint="11" * 32,
    )
    base.update(over)
    return S.WarpParams(**base)


def build(store, *, params=None, evm=None, coinset=None, watcher=None,
          wallet=None, collector=None, protector=None, clock=None):
    evm = evm or FakeEvm()
    coinset = coinset or FakeCoinset()
    watcher = watcher or FakeWatcher()
    wallet = wallet or FakeWallet()
    collector = collector or FakeCollector(
        nostr.SigResult(collected={}, threshold=NET.signature_threshold)
    )
    protector = protector or FakeProtector()
    key = SimpleNamespace(address="0x" + "ab" * 20, private_key=bytes(range(32)))
    engine = S.WarpEngine(
        NET, store,
        params=params or default_params(),
        evm=evm, coinset=coinset, watcher=watcher, wallet=wallet,
        collector=collector, evm_key=key, protector=protector,
        clock=clock or big_clock(),
    )
    ctx = SimpleNamespace(
        engine=engine, store=store, evm=evm, coinset=coinset, watcher=watcher,
        wallet=wallet, collector=collector, protector=protector, key=key,
    )
    return engine, ctx


def make_ephemeral_blob(job_id: int):
    """A valid protected ephemeral BLS blob bound to ``job_id`` (as the engine wraps it)."""
    bls = keystore.new_bls_key()
    blob = keystore.protect_bls_key(
        bls, extra_entropy=f"warp-job-{job_id}".encode(), protector=FakeProtector()
    )
    return blob, bls.private_key


# The hot-wallet address baked into build()'s fake EVM key. Jobs must carry it,
# or WarpEngine._binding_mismatch refuses to process them (see F8): a job past
# AWAITING_DEPOSIT with no recorded hot wallet cannot be proven to belong to the
# configured key. Without this every seeded test would silently no-op.
HOT_ADDRESS = "0x" + "ab" * 20


def seed(store, status, *, columns=None, state=None):
    merged = {"network": NET.name, "hot_address": "ab" * 20}
    merged.update(state or {})
    return store.create_job(NET.name, status=status, columns=columns, state=merged)


# --------------------------------------------------------------------------- #
# AWAITING_DEPOSIT.
# --------------------------------------------------------------------------- #

def test_awaiting_deposit_freezes_amounts_and_advances():
    store = new_store()
    engine, ctx = build(store)
    ctx.evm.erc20 = 5_000_000  # 5 USDC
    seed(store, JobStatus.AWAITING_DEPOSIT, state={"manual": True})

    out = engine.step()

    assert out["status"] == JobStatus.DEPOSIT_SEEN
    job = store.get_active_job()
    assert job.amount_usdc_micros == 5_000_000
    assert job.amount_mojos == 5000                 # 6->3 decimal factor 1000
    assert job.post_tip_mojos == 4985              # 0.3% of 5000 = 15
    assert job.receiver_address == ADDR
    assert job.receiver_ph == RECEIVER_PH.hex()
    assert job.state["tip_bps"] == 30
    # Advance clears error bookkeeping and does not schedule a backoff.
    assert job.retry_count == 0 and job.last_error is None and job.next_retry_at is None


def test_awaiting_deposit_below_min_stays_pending_without_retry_bump():
    store = new_store()
    engine, ctx = build(store)
    ctx.evm.erc20 = 500_000  # below the 1_000_000 min
    seed(store, JobStatus.AWAITING_DEPOSIT)

    out = engine.step()

    assert out["status"] == JobStatus.AWAITING_DEPOSIT
    job = store.get_active_job()
    assert job.retry_count == 0            # pending is not a retry
    assert job.last_error is None
    assert job.next_retry_at is not None   # but a stay does schedule the next poll


def test_awaiting_deposit_low_gas_is_retryable_backoff():
    store = new_store()
    engine, ctx = build(store)
    ctx.evm.erc20 = 5_000_000
    ctx.evm.eth = S._MIN_GAS_WEI - 1  # deposit present but cannot pay gas
    seed(store, JobStatus.AWAITING_DEPOSIT)

    engine.step()

    job = store.get_active_job()
    assert job.status == JobStatus.AWAITING_DEPOSIT
    assert job.retry_count == 1
    assert job.last_error and "ETH" in job.last_error
    assert job.next_retry_at is not None


def test_client_error_folds_into_retry_not_terminal():
    store = new_store()
    engine, ctx = build(store)
    ctx.evm.raise_erc20 = RuntimeError("RPC 502 bad gateway")
    seed(store, JobStatus.AWAITING_DEPOSIT)

    engine.step()

    job = store.get_active_job()
    assert job.status == JobStatus.AWAITING_DEPOSIT   # not FAILED
    assert job.retry_count == 1
    assert "502" in (job.last_error or "")


# --------------------------------------------------------------------------- #
# DEPOSIT_SEEN preflight.
# --------------------------------------------------------------------------- #

def test_deposit_seen_advances_to_approving():
    store = new_store()
    engine, _ = build(store)
    seed(store, JobStatus.DEPOSIT_SEEN, columns={"receiver_ph": RECEIVER_PH.hex()})

    out = engine.step()

    assert out["status"] == JobStatus.APPROVING


def test_deposit_seen_invalid_receiver_ph_fails_closed():
    store = new_store()
    engine, _ = build(store)
    seed(store, JobStatus.DEPOSIT_SEEN, columns={"receiver_ph": "00" * 31})  # 31 bytes

    out = engine.step()

    assert out["status"] == JobStatus.FAILED
    job = store.get_job(out["id"])
    assert job.state["failed_from"] == JobStatus.DEPOSIT_SEEN


# --------------------------------------------------------------------------- #
# APPROVING (allowance skip + two-phase, no double-sign).
# --------------------------------------------------------------------------- #

def test_approving_skips_when_allowance_sufficient():
    store = new_store()
    engine, ctx = build(store)
    ctx.evm.allowance = 5000
    seed(store, JobStatus.APPROVING, columns={"amount_usdc_micros": 1000})

    out = engine.step()

    assert out["status"] == JobStatus.BRIDGING
    assert ctx.evm.prepared == []       # nothing signed when allowance already covers it


def test_approving_two_phase_signs_once_rebroadcasts_identical(monkeypatch):
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    ctx.evm.allowance = 0
    seed(store, JobStatus.APPROVING, columns={"amount_usdc_micros": 5_000_000})

    # Tick 1: sign + persist raw, NO broadcast.
    engine.step()
    job = store.get_active_job()
    assert job.status == JobStatus.APPROVING
    assert job.state["approve_raw"] == b"\x02\xaa\xaa\xaa".hex()
    assert ctx.evm.sent_raw == []
    assert ctx.evm.prepared == [("approve", 5_000_000)]

    # Tick 2: broadcast, receipt not yet available -> stay.
    ctx.evm.receipt = None
    engine.step()
    assert store.get_active_job().status == JobStatus.APPROVING
    assert ctx.evm.sent_raw == [b"\x02\xaa\xaa\xaa"]

    # Tick 3 (crash-resume): same raw re-broadcast, receipt now succeeds -> advance.
    ctx.evm.receipt = {"status": "0x1"}
    out = engine.step()
    assert out["status"] == JobStatus.BRIDGING
    assert ctx.evm.sent_raw == [b"\x02\xaa\xaa\xaa", b"\x02\xaa\xaa\xaa"]  # identical
    assert ctx.evm.prepared == [("approve", 5_000_000)]                    # signed only once


def test_approving_revert_reprepares_until_cap(monkeypatch):
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    ctx.evm.allowance = 0
    seed(store, JobStatus.APPROVING, columns={"amount_usdc_micros": 5_000_000})

    engine.step()                       # phase 1: sign
    ctx.evm.receipt = {"status": "0x0"}  # reverted
    engine.step()                       # phase 2: broadcast -> revert -> clear raw

    job = store.get_active_job()
    assert job.status == JobStatus.APPROVING
    assert job.state["approve_raw"] is None
    assert job.state["approve_attempt"] == 1


# --------------------------------------------------------------------------- #
# BRIDGING (two-phase + MessageSent nonce parse).
# --------------------------------------------------------------------------- #

def test_bridging_two_phase_parses_message_nonce(monkeypatch):
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    seed(store, JobStatus.BRIDGING,
         columns={"amount_mojos": 5000, "receiver_ph": RECEIVER_PH.hex()})

    # Phase 1: sign, persist raw + tx hash, no broadcast.
    engine.step()
    job = store.get_active_job()
    assert job.status == JobStatus.BRIDGING
    assert job.bridge_tx_hash == "0x" + "22" * 32
    assert job.state["bridge_raw"] == b"\x02\xbb\xbb\xbb".hex()
    assert ctx.evm.sent_raw == []

    # Phase 2: broadcast, receipt with MessageSent -> parse nonce, advance.
    nonce_hex = "00" * 31 + "07"
    ctx.evm.receipt = message_sent_receipt(nonce_hex)
    out = engine.step()
    assert out["status"] == JobStatus.BRIDGE_CONFIRMED
    assert store.get_active_job().bridge_nonce == nonce_hex
    assert ctx.evm.sent_raw == [b"\x02\xbb\xbb\xbb"]


def test_bridging_revert_reprepares(monkeypatch):
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    seed(store, JobStatus.BRIDGING,
         columns={"amount_mojos": 5000, "receiver_ph": RECEIVER_PH.hex()})

    engine.step()                       # sign
    ctx.evm.receipt = {"status": "0x0"}  # reverted
    engine.step()                       # broadcast -> revert -> clear raw + tx hash

    job = store.get_active_job()
    assert job.status == JobStatus.BRIDGING
    assert job.state["bridge_raw"] is None
    assert job.bridge_tx_hash is None
    assert job.state["bridge_attempt"] == 1


# --------------------------------------------------------------------------- #
# BRIDGE_CONFIRMED confirmation gate.
# --------------------------------------------------------------------------- #

def test_bridge_confirmed_waits_then_advances():
    store = new_store()
    engine, ctx = build(store)
    seed(store, JobStatus.BRIDGE_CONFIRMED,
         columns={"bridge_tx_hash": "0x" + "22" * 32, "bridge_nonce": "00" * 31 + "07"})

    ctx.evm.confs = NET.evm_confirmation_min_height - 1
    assert engine.step()["status"] == JobStatus.BRIDGE_CONFIRMED  # not deep enough

    ctx.evm.confs = NET.evm_confirmation_min_height
    assert engine.step()["status"] == JobStatus.MESSAGE_SENT


# --------------------------------------------------------------------------- #
# MESSAGE_SENT (attestation anchors, then key mint).
# --------------------------------------------------------------------------- #

# The real watcher returns every message content as a bytes32 word, so the
# ERC-20 source arrives 12 zero bytes wider than net.usdc_address. Verified
# against a live mainnet attestation (nonce ...01d7):
#   contents[0] == "000000000000000000000000833589fcd6edb6e08f4c7c32d4f71b54bda02913"
# This fixture previously used the bare 20-byte form, which is why the
# always-unequal comparison in _h_message_sent went unnoticed.
_USDC_WORD = _hx_pad32(NET.usdc_address)


def _sent_msg(**over):
    fields = dict(
        is_sent=True,
        status="sent",
        receiver_ph=RECEIVER_PH.hex(),
        amount_mojos=4985,
        erc20_source=_USDC_WORD,
        destination=bytes(32),
        contents=[_USDC_WORD, RECEIVER_PH.hex(), "1379"],
    )
    fields.update(over)
    return SimpleNamespace(**fields)


def _seed_message_sent(store):
    return seed(
        store, JobStatus.MESSAGE_SENT,
        columns={"receiver_ph": RECEIVER_PH.hex(), "post_tip_mojos": 4985,
                 "bridge_nonce": "00" * 31 + "07"},
    )


def test_message_sent_anchor_match_generates_key():
    store = new_store()
    engine, ctx = build(store, watcher=FakeWatcher(_sent_msg()))
    _seed_message_sent(store)

    out = engine.step()

    assert out["status"] == JobStatus.FUNDING_CLAIM
    job = store.get_active_job()
    assert job.state["ephemeral_blob"]                       # key persisted with the advance
    assert job.state["message_destination"] == "00" * 32
    assert job.state["message_contents"] == ["aa" * 20, RECEIVER_PH.hex(), "1379"]
    # No CAT-coin baseline is taken any more: third-party-claim detection is
    # per-nonce (see test_claiming_conflict_*), not a count-versus-baseline.
    assert "wrapped_cat_baseline" not in job.state
    # The persisted blob is a real, decryptable ephemeral key bound to this job.
    sk = engine._load_ephemeral_sk(job)
    assert len(sk) == 32


def test_message_sent_pending_until_sent():
    store = new_store()
    engine, _ = build(store, watcher=FakeWatcher(_sent_msg(is_sent=False, status="pending")))
    _seed_message_sent(store)

    out = engine.step()

    assert out["status"] == JobStatus.MESSAGE_SENT   # still waiting on attestation
    assert store.get_active_job().retry_count == 0   # pending, not an error


def test_message_sent_receiver_mismatch_fails_closed():
    store = new_store()
    bad = _sent_msg(receiver_ph=("ff" * 32))
    engine, _ = build(store, watcher=FakeWatcher(bad))
    _seed_message_sent(store)

    out = engine.step()

    assert out["status"] == JobStatus.FAILED
    assert store.get_job(out["id"]).state["failed_from"] == JobStatus.MESSAGE_SENT


def test_message_sent_amount_mismatch_fails_closed():
    store = new_store()
    engine, _ = build(store, watcher=FakeWatcher(_sent_msg(amount_mojos=4984)))
    _seed_message_sent(store)

    out = engine.step()

    assert out["status"] == JobStatus.FAILED
    assert store.get_job(out["id"]).state["failed_from"] == JobStatus.MESSAGE_SENT


# --------------------------------------------------------------------------- #
# FUNDING_CLAIM (dedupe-scan, never double-fund).
# --------------------------------------------------------------------------- #

def test_funding_claim_dedupes_and_never_double_sends(monkeypatch):
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    monkeypatch.setattr(claim_mod, "security_coin_puzzle_hash", lambda sk: bytes(32))
    store = new_store()
    engine, ctx = build(store)
    job = seed(store, JobStatus.FUNDING_CLAIM, columns={"post_tip_mojos": 4985})
    blob, _ = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob})

    expected = 4985 + 100_000_000

    # Tick 1: nothing on chain, no in-flight tx -> send exactly once.
    engine.step()
    assert len(ctx.wallet.sent) == 1
    assert ctx.wallet.sent[0][0] == expected
    assert store.get_active_job().state["funding_tx_id"] == "funding-tx-1"

    # Tick 2 (crash-resume): the funding now shows in wallet history -> dedupe, NO re-send.
    ctx.wallet.txs = [{
        "name": "funding-tx-1",
        "additions": [{"puzzle_hash": bytes(32).hex(), "amount": expected}],
    }]
    engine.step()
    assert len(ctx.wallet.sent) == 1   # still one; the dedupe-scan suppressed a second send


def test_funding_claim_finds_coin_and_advances(monkeypatch):
    coin = FakeCoin(b"\x5a" * 32)
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: coin)
    store = new_store()
    engine, _ = build(store)
    job = seed(store, JobStatus.FUNDING_CLAIM, columns={"post_tip_mojos": 4985})
    blob, _ = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob})

    out = engine.step()

    assert out["status"] == JobStatus.CLAIM_FUNDED
    assert store.get_active_job().state["security_coin_id"] == (b"\x5a" * 32).hex()


# --------------------------------------------------------------------------- #
# CLAIM_FUNDED confirmation gate.
# --------------------------------------------------------------------------- #

def test_claim_funded_depth_gate(monkeypatch):
    coin = FakeCoin(b"\x5a" * 32)
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: coin)
    store = new_store()
    engine, ctx = build(store)
    ctx.coinset.record = SimpleNamespace(confirmed_block_index=100)
    job = seed(store, JobStatus.CLAIM_FUNDED, columns={"post_tip_mojos": 4985})
    blob, _ = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob})

    ctx.coinset.peak = 100  # depth 1 < 32
    assert engine.step()["status"] == JobStatus.CLAIM_FUNDED

    ctx.coinset.peak = 100 + NET.chia_confirmation_min_height - 1  # depth == 32
    out = engine.step()
    assert out["status"] == JobStatus.COLLECTING_SIGS
    assert store.get_active_job().state["security_coin_id"] == (b"\x5a" * 32).hex()


# --------------------------------------------------------------------------- #
# COLLECTING_SIGS (portal freshness + incremental persist).
# --------------------------------------------------------------------------- #

PORTAL = b"\x11" * 32
NEW_PORTAL = b"\x22" * 32


def _patch_portal(monkeypatch, coin_id=PORTAL):
    monkeypatch.setattr(
        claim_mod, "sync_portal",
        lambda coinset, net, *, hint, max_hops=64: SimpleNamespace(coin_id=coin_id),
    )
    monkeypatch.setattr(claim_mod, "validator_digest", lambda *a, **k: b"\x00" * 32)


def _seed_collecting(store, **state):
    base = {"message_destination": "00" * 32, "message_contents": ["aa", "bb", "cc"]}
    base.update(state)
    return seed(store, JobStatus.COLLECTING_SIGS,
                columns={"bridge_nonce": "00" * 31 + "07", "post_tip_mojos": 4985,
                         "receiver_ph": RECEIVER_PH.hex()},
                state=base)


def test_collecting_sigs_incomplete_persists_and_rounds(monkeypatch):
    _patch_portal(monkeypatch)
    store = new_store()
    collector = FakeCollector(nostr.SigResult(collected={0: b"s0"}, threshold=6))
    engine, ctx = build(store, collector=collector)
    _seed_collecting(store)

    out = engine.step()

    assert out["status"] == JobStatus.COLLECTING_SIGS
    job = store.get_active_job()
    assert job.state["sigs"] == {"0": b"s0".hex()}       # incremental persist
    assert job.state["portal_coin_id"] == PORTAL.hex()
    assert job.state["collect_round"] == 1
    assert store.get_meta("portal_hint") == PORTAL.hex()  # hint advanced for next resume


def test_collecting_sigs_portal_advance_wipes_stale_sigs(monkeypatch):
    _patch_portal(monkeypatch, coin_id=NEW_PORTAL)
    store = new_store()
    collector = FakeCollector(nostr.SigResult(collected={}, threshold=6))
    engine, ctx = build(store, collector=collector)
    _seed_collecting(store, portal_coin_id=PORTAL.hex(), sigs={"9": "abcd"})

    engine.step()

    # The portal moved under us, so the collector must start from an empty set.
    assert ctx.collector.calls[0]["have"] == {}
    assert store.get_active_job().state["portal_coin_id"] == NEW_PORTAL.hex()


def test_collecting_sigs_resume_same_portal_preserves_have_and_completes(monkeypatch):
    _patch_portal(monkeypatch)
    store = new_store()
    full = {i: bytes([0xC0 + i]) for i in range(6)}
    collector = FakeCollector(nostr.SigResult(collected=full, threshold=6))
    engine, ctx = build(store, collector=collector)
    _seed_collecting(store, portal_coin_id=PORTAL.hex(), sigs={"0": "aa"})

    out = engine.step()

    # Same portal: previously collected sig is carried into the collector.
    assert ctx.collector.calls[0]["have"] == {0: b"\xaa"}
    assert out["status"] == JobStatus.CLAIMING
    job = store.get_active_job()
    assert len(job.state["sigs"]) == 6
    assert job.state["portal_coin_id"] == PORTAL.hex()


# --------------------------------------------------------------------------- #
# CLAIMING (build+push, completion, portal drift).
# --------------------------------------------------------------------------- #

FINAL_CAT = b"\xff" * 32


def _seed_claiming(store, job_id_holder=None, **state):
    base = {
        "message_destination": "00" * 32,
        "message_contents": ["aa", "bb", "cc"],
        "portal_coin_id": PORTAL.hex(),
        "sigs": {str(i): bytes([0xC0 + i]).hex() for i in range(6)},
        "security_coin_id": (b"\x5a" * 32).hex(),
    }
    base.update(state)
    job = seed(store, JobStatus.CLAIMING,
               columns={"bridge_nonce": "00" * 31 + "07", "post_tip_mojos": 4985,
                        "receiver_ph": RECEIVER_PH.hex()},
               state=base)
    blob, _ = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob})
    return store.get_active_job()


def test_claiming_pushes_then_completes(monkeypatch):
    coin = FakeCoin(b"\x5a" * 32)
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: coin)
    monkeypatch.setattr(
        claim_mod, "sync_portal",
        lambda coinset, net, *, hint, max_hops=64: SimpleNamespace(coin_id=PORTAL),
    )
    monkeypatch.setattr(claim_mod, "claim_landed", lambda coinset, cid: True)

    def fake_push(coinset, net, **kw):
        return SimpleNamespace(
            claim=SimpleNamespace(final_cat_coin_id=FINAL_CAT),
            accepted=True, status="accepted",
        )

    monkeypatch.setattr(claim_mod, "build_and_push_claim", fake_push)

    store = new_store()
    engine, _ = build(store)
    _seed_claiming(store)

    # Tick 1: build + push; final not yet on chain -> stay, predicted id persisted.
    out1 = engine.step()
    assert out1["status"] == JobStatus.CLAIMING
    job = store.get_active_job()
    assert job.state["final_cat_coin_id"] == FINAL_CAT.hex()
    assert job.state["security_coin_id"] == (b"\x5a" * 32).hex()

    # Tick 2 (resume): the predicted final CAT coin exists -> COMPLETED.
    out2 = engine.step()
    assert out2["status"] == JobStatus.COMPLETED


def test_claiming_portal_drift_recollects_and_wipes_sigs(monkeypatch):
    coin = FakeCoin(b"\x5a" * 32)
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: coin)
    monkeypatch.setattr(claim_mod, "claim_landed", lambda coinset, cid: False)
    monkeypatch.setattr(
        claim_mod, "sync_portal",
        lambda coinset, net, *, hint, max_hops=64: SimpleNamespace(coin_id=NEW_PORTAL),
    )

    store = new_store()
    engine, _ = build(store)
    _seed_claiming(store)  # persisted portal_coin_id == PORTAL, but chain moved to NEW_PORTAL

    out = engine.step()

    assert out["status"] == JobStatus.COLLECTING_SIGS
    job = store.get_active_job()
    assert job.state["sigs"] is None                       # stale sigs wiped
    assert job.state["portal_coin_id"] == NEW_PORTAL.hex()


# --------------------------------------------------------------------------- #
# Dispatcher: backoff gate + terminal noop.
# --------------------------------------------------------------------------- #

def test_backoff_gate_blocks_handler_within_window():
    store = new_store()
    engine, ctx = build(store)
    ctx.evm.erc20 = 5_000_000
    seed(store, JobStatus.AWAITING_DEPOSIT,
         columns={"next_retry_at": repr(1e18)})  # far beyond the big-clock's ~1e5

    out = engine.step()

    assert out["status"] == JobStatus.AWAITING_DEPOSIT
    assert ctx.evm.erc20_calls == 0   # handler never ran -- the gate returned first


def test_failed_job_is_a_noop():
    store = new_store()
    engine, ctx = build(store)
    seed(store, JobStatus.FAILED, columns={"receiver_ph": RECEIVER_PH.hex()},
         state={"failed_from": JobStatus.DEPOSIT_SEEN})

    out = engine.step()

    assert out["status"] == JobStatus.FAILED
    assert ctx.evm.erc20_calls == 0   # terminal states hold the slot without advancing


# --------------------------------------------------------------------------- #
# Public API: auto-start + manual request guard.
# --------------------------------------------------------------------------- #

def test_maybe_start_auto_job_opens_on_threshold():
    store = new_store()
    params = default_params(enabled=True, auto_bridge=True, min_micros=1_000_000)
    engine, ctx = build(store, params=params)
    ctx.evm.erc20 = 5_000_000

    opened = engine.maybe_start_auto_job()

    assert opened is not None
    active = store.get_active_job()
    assert active.status == JobStatus.AWAITING_DEPOSIT
    assert active.state.get("auto") is True


def test_maybe_start_auto_job_noop_when_disabled():
    store = new_store()
    engine, ctx = build(store)          # default params: enabled=False
    ctx.evm.erc20 = 5_000_000

    assert engine.maybe_start_auto_job() is None
    assert store.get_active_job() is None


def test_maybe_start_auto_job_noop_below_threshold():
    store = new_store()
    params = default_params(enabled=True, auto_bridge=True, min_micros=1_000_000)
    engine, ctx = build(store, params=params)
    ctx.evm.erc20 = 999_999

    assert engine.maybe_start_auto_job() is None
    assert store.get_active_job() is None


def test_request_bridge_rejects_second_active_job():
    store = new_store()
    engine, _ = build(store)
    seed(store, JobStatus.AWAITING_DEPOSIT)

    with pytest.raises(S.WarpError):
        engine.request_bridge()


def test_request_bridge_records_target_micros():
    store = new_store()
    engine, _ = build(store)

    engine.request_bridge(target_micros=2_500_000)

    job = store.get_active_job()
    assert job.status == JobStatus.AWAITING_DEPOSIT
    assert job.state["target_micros"] == 2_500_000
    assert job.state["manual"] is True


# --------------------------------------------------------------------------- #
# Operator actions.
# --------------------------------------------------------------------------- #

def test_job_action_cancel_only_before_bridge():
    store = new_store()
    engine, _ = build(store)
    job = seed(store, JobStatus.APPROVING, columns={"amount_usdc_micros": 1000})

    out = engine.job_action(job.id, "cancel")
    assert out["status"] == JobStatus.CANCELLED
    # Slot freed once cancelled.
    assert store.get_active_job() is None


def test_job_action_cancel_rejected_after_bridge():
    store = new_store()
    engine, _ = build(store)
    job = seed(store, JobStatus.BRIDGE_CONFIRMED,
               columns={"bridge_nonce": "00" * 31 + "07"})

    with pytest.raises(S.WarpError):
        engine.job_action(job.id, "cancel")


def test_job_action_retry_rewinds_failed_to_origin():
    store = new_store()
    engine, _ = build(store)
    job = seed(store, JobStatus.FAILED,
               columns={"receiver_ph": RECEIVER_PH.hex(), "last_error": "boom"},
               state={"failed_from": JobStatus.COLLECTING_SIGS})

    out = engine.job_action(job.id, "retry")

    assert out["status"] == JobStatus.COLLECTING_SIGS
    refreshed = store.get_active_job()
    assert refreshed.last_error is None and refreshed.retry_count == 0
