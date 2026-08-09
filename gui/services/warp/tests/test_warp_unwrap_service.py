"""Outbound (unwrap) state-machine tests.

Extends the shared harness with outbound fakes. The commit point -- the
``cat_spend`` in BURN_SENT -- gets the same guard battery the inbound funding
send earned the hard way: send exactly once, marker holds, history scan fails
closed, completion is idempotent.
"""

from __future__ import annotations

from types import SimpleNamespace

import pytest

pytest.importorskip("clvm")
pytest.importorskip("chia_rs")

from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import nostr as nostr_mod  # noqa: E402
from gui.services.warp import service as S  # noqa: E402
from gui.services.warp.jobs import JobStatus  # noqa: E402

from .test_warp_service import (  # noqa: E402
    FakeCoin,
    FakeEvm,
    FakeWallet,
    build,
    default_params,
    new_store,
)

NET = C.MAINNET
RECEIVER = "0x" + "ab" * 20          # all-lowercase: no EIP-55 gate
AMOUNT = 5000                        # 5 USDC in CAT mojos


class UnwrapFakeEvm(FakeEvm):
    def __init__(self) -> None:
        super().__init__()
        self.burn_ph = bytes.fromhex(NET.burn_puzzle_hash)
        self.threshold = 6
        self.domain_ok = True
        self.bridge_liquidity = 10 ** 12
        self.relay_receipts: dict = {}

    def get_burn_puzzle_hash(self):
        return self.burn_ph

    def verify_eip712_domain(self):
        from gui.services.warp import evm as evm_mod

        if not self.domain_ok:
            raise evm_mod.EvmError("portal EIP-712 domain mismatch ... redeploy")
        return bytes.fromhex(NET.eip712_domain_separator)

    def get_signature_threshold(self):
        return self.threshold

    def get_erc20_balance(self, token, owner):
        # The gate reads the BRIDGE's liquidity; the inbound fake reads the
        # hot wallet. Split on the owner.
        if owner == NET.erc20_bridge_address:
            return self.bridge_liquidity
        return super().get_erc20_balance(token, owner)

    def prepare_relay(self, *, owner, calldata, nonce=None, fees=None):
        return SimpleNamespace(
            kind="relay", data=calldata,
            nonce=7 if nonce is None else nonce,
            max_fee_per_gas=fees.max_fee_per_gas if fees else 2_000_000_000,
            max_priority_fee_per_gas=(
                fees.max_priority_fee_per_gas if fees else 1_000_000
            ),
        )


class UnwrapFakeWallet(FakeWallet):
    def __init__(self) -> None:
        super().__init__()
        self.cat_wallet_id = 3
        self.balances = {
            1: {"confirmed_wallet_balance": 10 ** 12, "spendable_balance": 10 ** 12},
            3: {"confirmed_wallet_balance": 50_000, "spendable_balance": 50_000},
        }
        self.cat_spends: list = []
        self.cat_txs: list = []

    def get_wallets(self, *, wallet_type=None):
        return [{"id": self.cat_wallet_id, "type": 6, "name": "wUSDC.b"}]

    def cat_get_asset_id(self, wallet_id):
        return "0x" + NET.expected_asset_id      # live daemon 0x-prefixes it

    def get_wallet_balance(self, wallet_id):
        return dict(self.balances[int(wallet_id)])

    def cat_spend(self, wallet_id, amount, inner_address, *, fee_mojos=0, memos=None):
        self.cat_spends.append((wallet_id, amount, inner_address, fee_mojos))
        return {"name": f"burn-tx-{len(self.cat_spends)}"}

    def get_transactions(self, wallet_id=1, *, start=0, end=50, reverse=True,
                         confirmed=None):
        if int(wallet_id) == self.cat_wallet_id:
            if confirmed is False:
                return [t for t in self.cat_txs if not t.get("confirmed")]
            return list(self.cat_txs)
        return super().get_transactions(
            wallet_id, start=start, end=end, reverse=reverse,
            confirmed=confirmed,
        )


def unwrap_sign_tx(unsigned, key):
    kind = getattr(unsigned, "kind", None) or (
        unsigned.get("kind") if isinstance(unsigned, dict) else "x"
    )
    if kind == "relay":
        return SimpleNamespace(raw=b"\x02\xee\xee\xee", tx_hash="0x" + "77" * 32,
                               nonce=unsigned.nonce)
    if kind == "bridge":
        return SimpleNamespace(raw=b"\x02\xbb\xbb\xbb", tx_hash="0x" + "22" * 32, nonce=3)
    return SimpleNamespace(raw=b"\x02\xaa\xaa\xaa", tx_hash="0x" + "11" * 32, nonce=0)


def build_unwrap(store, *, params=None, collector=None):
    evm = UnwrapFakeEvm()
    wallet = UnwrapFakeWallet()
    engine, ctx = build(
        store, params=params or default_params(),
        evm=evm, wallet=wallet, collector=collector,
    )
    return engine, ctx


def request(engine, *, amount=AMOUNT, receiver=RECEIVER):
    return engine.request_unwrap(amount, receiver)


def params_with_cap(**over):
    over.setdefault("max_unwrap_micros", 100_000_000)   # $100
    return default_params(**over)


# --------------------------------------------------------------------------- #
# request_unwrap refusals: synchronous, no job created.
# --------------------------------------------------------------------------- #

def test_request_unwrap_refuses_until_the_cap_is_stated():
    store = new_store()
    engine, _ = build_unwrap(store)         # default_params: cap never set (-1)
    with pytest.raises(S.WarpError, match="max_unwrap_usdc is not set"):
        request(engine)
    assert store.get_active_job() is None


def test_request_unwrap_refuses_rather_than_clamps():
    store = new_store()
    engine, _ = build_unwrap(store, params=params_with_cap())
    with pytest.raises(S.WarpError, match="refusing rather than clamping"):
        request(engine, amount=200_000)     # $200 > $100 cap
    assert store.get_active_job() is None


def test_request_unwrap_validates_the_receiver_hard():
    store = new_store()
    engine, _ = build_unwrap(store, params=params_with_cap())
    for bad, why in [
        ("0x1234", "20-byte"),
        ("0x" + "00" * 20, "zero address"),
        ("0x" + "gg" * 20, "not valid hex"),
        # Mixed case with a broken EIP-55 checksum.
        ("0xAb" + "cd" * 19, "EIP-55"),
    ]:
        with pytest.raises(S.WarpError, match=why):
            request(engine, receiver=bad)
    assert store.get_active_job() is None


def test_request_unwrap_freezes_binding_and_direction():
    store = new_store()
    engine, _ = build_unwrap(store, params=params_with_cap())
    out = request(engine)
    job = store.get_active_job()
    assert out["status"] == JobStatus.UNWRAP_CHECKS
    assert job.state["direction"] == "out"
    assert job.state["dry_run"] is False        # frozen from params at creation
    assert job.state["receiver_evm"] == RECEIVER[2:]
    assert job.amount_usdc_micros == AMOUNT * 1000


# --------------------------------------------------------------------------- #
# UNWRAP_CHECKS: gates and the rehearsal stop.
# --------------------------------------------------------------------------- #

def test_dry_run_stops_before_the_cat_spend():
    """The rehearsal exercises every gate and never reaches the commit point."""
    store = new_store()
    engine, ctx = build_unwrap(
        store, params=params_with_cap(dry_run=True)
    )
    request(engine)
    out = engine.step()

    assert out["status"] == JobStatus.DRY_RUN_OK
    assert ctx.wallet.cat_spends == [], "a rehearsal must never cat_spend"
    assert store.get_active_job() is None       # closed; the slot is free


def test_a_burn_anchor_mismatch_is_terminal():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    ctx.evm.burn_ph = b"\x99" * 32              # live hash disagrees
    request(engine)
    out = engine.step()
    assert out["status"] == JobStatus.FAILED
    assert "redeploy" in (out["last_error"] or "")
    assert ctx.wallet.cat_spends == []


def test_offer_locked_balance_pends_rather_than_burning_collateral():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    ctx.wallet.balances[3] = {
        "confirmed_wallet_balance": 50_000, "spendable_balance": AMOUNT - 1,
    }
    request(engine)
    out = engine.step()
    assert out["status"] == JobStatus.UNWRAP_CHECKS
    assert out["retry_count"] == 0, "a healthy pend, not an error"
    assert store.get_active_job().state["pending_polls"] == 1
    assert ctx.wallet.cat_spends == []

    # Collateral frees (offers closed) -> the same job proceeds.
    ctx.wallet.balances[3]["spendable_balance"] = 50_000
    assert engine.step()["status"] == JobStatus.BURN_SENT


def test_gates_pass_and_advance_to_burn_sent():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    request(engine)
    out = engine.step()

    assert out["status"] == JobStatus.BURN_SENT
    job = store.get_active_job()
    assert job.state["cat_wallet_id"] == 3
    assert job.state["post_tip_base_units"] == 4_985_000    # 5 USDC - 30bps
    assert job.state["burn_address"].startswith("xch1")
    assert ctx.wallet.cat_spends == [], "gates must not spend"


# --------------------------------------------------------------------------- #
# BURN_SENT: the commit point's guard battery.
# --------------------------------------------------------------------------- #

def _to_burn_sent(store, engine, ctx):
    request(engine)
    assert engine.step()["status"] == JobStatus.BURN_SENT


def test_the_cat_spend_happens_exactly_once():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    _to_burn_sent(store, engine, ctx)

    engine.step()                                # sends
    assert len(ctx.wallet.cat_spends) == 1
    wid, amount, address, fee = ctx.wallet.cat_spends[0]
    assert (wid, amount) == (3, AMOUNT)
    assert address == store.get_active_job().state["burn_address"]

    engine.step()                                # marker holds
    engine.step()
    assert len(ctx.wallet.cat_spends) == 1, "the commit point must fire once"


def test_a_failed_history_scan_refuses_the_cat_spend():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    _to_burn_sent(store, engine, ctx)

    def boom(*a, **k):
        raise TimeoutError("wallet RPC timed out")

    ctx.wallet.get_transactions = boom
    out = engine.step()
    assert ctx.wallet.cat_spends == []
    assert "refusing to send the irreversible cat_spend" in (out["last_error"] or "")


def test_a_history_hit_is_trusted_over_resending():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    _to_burn_sent(store, engine, ctx)
    burn_ph = store.get_active_job().state["burn_cat_ph"]
    ctx.wallet.cat_txs = [
        {"name": "prior-burn", "confirmed": False,
         "additions": [{"puzzle_hash": "0x" + burn_ph, "amount": AMOUNT,
                        "parent_coin_info": "0x" + "55" * 32}]}
    ]

    engine.step()
    assert ctx.wallet.cat_spends == []
    state = store.get_active_job().state
    assert state["burn_tx_id"] == "prior-burn"
    # The scan yields provenance: the exact coin id the completion scan pins.
    from gui.services.warp import clvm_utils as cu

    assert state["burn_cat_coin_id"] == cu.coin_name(
        b"\x55" * 32, bytes.fromhex(burn_ph), AMOUNT
    ).hex()


def test_a_nameless_cat_spend_still_arms_the_guard():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    _to_burn_sent(store, engine, ctx)

    def unnamed(wallet_id, amount, inner_address, *, fee_mojos=0, memos=None):
        ctx.wallet.cat_spends.append((wallet_id, amount, inner_address, fee_mojos))
        return {"status": "SUCCESS"}

    ctx.wallet.cat_spend = unnamed
    engine.step()
    assert store.get_active_job().state["burn_tx_id"] == S._FUNDING_TX_UNKNOWN
    engine.step()
    assert len(ctx.wallet.cat_spends) == 1


def test_burn_sent_advances_when_the_coin_appears():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    _to_burn_sent(store, engine, ctx)
    burn_ph = store.get_active_job().state["burn_cat_ph"]
    ctx.coinset.by_ph = [
        SimpleNamespace(
            coin=SimpleNamespace(parent_coin_info="44" * 32,
                                 puzzle_hash=burn_ph, amount=AMOUNT),
            spent=False, confirmed_block_index=100, spent_block_index=0,
        )
    ]
    out = engine.step()
    assert out["status"] == JobStatus.BURNING
    assert ctx.wallet.cat_spends == [], "completion must not send"


# --------------------------------------------------------------------------- #
# BURNING: the offline nonce is persisted before the push.
# --------------------------------------------------------------------------- #

def test_the_nonce_survives_a_crash_between_persist_and_push(monkeypatch):
    """The bundle push can die AFTER the nonce is persisted; the resumed job
    must poll the SAME nonce -- it is a pure function of the security coin."""
    from gui.services.warp import claim as claim_mod
    from gui.services.warp import drivers

    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    _to_burn_sent(store, engine, ctx)
    burn_ph = store.get_active_job().state["burn_cat_ph"]
    ctx.coinset.by_ph = [
        SimpleNamespace(
            coin=SimpleNamespace(parent_coin_info="44" * 32,
                                 puzzle_hash=burn_ph, amount=AMOUNT),
            spent=False, confirmed_block_index=100, spent_block_index=0,
        )
    ]
    engine.step()                                # -> BURNING
    engine.step()                                # mints the ephemeral key
    job = store.get_active_job()
    assert job.state.get("ephemeral_blob")

    sec = FakeCoin(b"\x5a" * 32)
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: sec)
    monkeypatch.setattr(
        claim_mod, "cat_lineage_proof_for",
        lambda coinset, parent: [b"\x33" * 32, b"\x44" * 32, AMOUNT],
    )
    fixed = SimpleNamespace(
        bundle=SimpleNamespace(to_json=lambda: {"coin_spends": []}),
        nonce=b"\xcd" * 32,
        cat_burner_coin=None, bridging_coin=None,
    )
    monkeypatch.setattr(drivers, "build_burn_bundle", lambda req: fixed)

    def push_dies(*a, **k):
        raise TimeoutError("node went away mid-push")

    monkeypatch.setattr(claim_mod, "push_burn_bundle", push_dies)
    out = engine.step()
    # The push failed as a retryable error -- but the nonce is already durable.
    assert store.get_active_job().state["unwrap_nonce"] == ("cd" * 32)


# --------------------------------------------------------------------------- #
# RELAYING: '!nonce' is success, and the happy path needs the event + depth.
# --------------------------------------------------------------------------- #

def _seed_relaying(store):
    from .test_warp_service import seed

    job = seed(
        store, JobStatus.RELAYING,
        columns={"amount_mojos": AMOUNT, "amount_usdc_micros": AMOUNT * 1000,
                 "bridge_nonce": "cd" * 32},
        state={"receiver_evm": "ab" * 20, "relay_sigs": "00" * 390,
               "relay_threshold": 6, "post_tip_base_units": 4_985_000},
    )
    return job


def test_a_third_party_delivery_completes_the_job(monkeypatch):
    from gui.services.warp import evm as evm_mod

    monkeypatch.setattr(evm_mod, "sign_tx", unwrap_sign_tx)
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    _seed_relaying(store)

    engine.step()                                # phase 1: sign

    def rejects(raw):
        raise evm_mod.EvmRpcError("execution reverted: !nonce")

    ctx.evm.send_raw_transaction = rejects
    out = engine.step()
    assert out["status"] == JobStatus.COMPLETED
    assert store.get_job(out["id"]).state["delivered_by"] == "a third-party relayer"


def test_our_relay_completes_only_with_the_event_and_depth(monkeypatch):
    from gui.services.warp import evm as evm_mod

    monkeypatch.setattr(evm_mod, "sign_tx", unwrap_sign_tx)
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    _seed_relaying(store)

    engine.step()                                # phase 1
    good_log = {
        "address": NET.portal_address,
        "topics": ["0x" + evm_mod.MESSAGE_RECEIVED_TOPIC0, "0x" + "cd" * 32],
    }
    ctx.evm.receipt = {"status": "0x1", "logs": [good_log]}

    ctx.evm.confs = NET.evm_confirmation_min_height - 1
    out = engine.step()
    assert out["status"] == JobStatus.RELAYING, "must wait for depth"

    ctx.evm.confs = NET.evm_confirmation_min_height
    out = engine.step()
    assert out["status"] == JobStatus.COMPLETED
    assert store.get_job(out["id"]).bridge_tx_hash == "0x" + "77" * 32

    # And a success receipt WITHOUT the Portal event must refuse to guess.
    store2 = new_store()
    engine2, ctx2 = build_unwrap(store2, params=params_with_cap())
    _seed_relaying(store2)
    engine2.step()
    ctx2.evm.receipt = {"status": "0x1", "logs": []}
    out2 = engine2.step()
    assert out2["status"] == JobStatus.FAILED
    assert "MessageReceived" in (out2["last_error"] or "")


# --------------------------------------------------------------------------- #
# Review findings, pinned.
# --------------------------------------------------------------------------- #

def test_the_dedupe_scan_sees_a_send_paged_out_of_the_history_window():
    """[UNWRAP-PENDING-SCAN] The daemon sorts confirmed-height descending and
    unconfirmed records sort LAST -- on the busy quote-asset wallet, the
    just-sent cat_spend pages out of any windowed scan, and the crash-window
    dedupe would burn TWICE. The unconfirmed set must be queried explicitly."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    _to_burn_sent(store, engine, ctx)
    burn_ph = store.get_active_job().state["burn_cat_ph"]

    in_flight = {"name": "in-flight", "confirmed": False,
                 "additions": [{"puzzle_hash": "0x" + burn_ph, "amount": AMOUNT,
                                "parent_coin_info": "0x" + "66" * 32}]}
    real_get = ctx.wallet.get_transactions

    def paged_out(wallet_id=1, *, start=0, end=50, reverse=True, confirmed=None):
        if confirmed is False:
            return [in_flight]
        # The default windowed view: 200 newer confirmed rows pushed it out.
        return [{"name": f"trade-{i}", "confirmed": True, "additions": []}
                for i in range(200)]

    ctx.wallet.get_transactions = paged_out
    engine.step()

    assert ctx.wallet.cat_spends == [], "the in-flight send must be found"
    assert store.get_active_job().state["burn_tx_id"] == "in-flight"


def test_sweep_never_closes_an_outbound_job(monkeypatch):
    """[UNWRAP-SWEEP-BURY] The no-key sweep shortcut read 'nothing was ever
    funded' over a live burn commitment and closed the job CANCELLED --
    recording a falsehood while real wUSDC.b sat forward-only at the burn
    puzzle hash. Outbound rows are closed by Abandon (which records the
    recovery blob), never by Sweep."""
    from .test_warp_service import seed

    store = new_store()
    engine, _ctx = build_unwrap(store, params=params_with_cap())
    job = seed(
        store, JobStatus.FAILED,
        columns={"amount_mojos": AMOUNT},
        state={"direction": "out", "failed_from": JobStatus.BURN_SENT,
               "burn_tx_id": "burn-tx-1", "burn_cat_ph": "aa" * 32},
    )

    out = engine.job_action(job.id, "sweep")

    assert out["status"] == JobStatus.FAILED, "sweep must not bury the burn"
    assert store.get_active_job() is not None
    assert "Abandon closes it" in (store.get_job(job.id).state.get("sweep_status") or "")


def test_burning_reuses_the_pinned_security_coin(monkeypatch):
    """[UNWRAP-COIN-PIN] The persisted nonce is a pure function of ONE
    security coin. Re-electing a different coin on a later tick would rebuild
    the bundle with a different bridging coin id than the nonce the job polls
    -- stalling forever on a value that can never appear."""
    from gui.services.warp import claim as claim_mod
    from gui.services.warp import drivers
    from .test_warp_service import seed, make_ephemeral_blob

    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = seed(
        store, JobStatus.BURNING,
        columns={"amount_mojos": AMOUNT},
        state={"direction": "out", "receiver_evm": "ab" * 20,
               "burn_cat_ph": "bb" * 32, "cat_wallet_id": 3},
    )
    blob, _sk = make_ephemeral_blob(job.id)
    pinned_parent = b"\x5a" * 32
    pinned = drivers.Coin(pinned_parent, b"\x5b" * 32, NET.chia_toll_mojos)
    store.update_job(job.id, state_patch={
        "ephemeral_blob": blob,
        "security_coin_id": pinned.name().hex(),
        "unwrap_nonce": "cd" * 32,
    })
    # The pinned coin exists unspent; a DIFFERENT coin also matches by ph+amount.
    other = drivers.Coin(b"\x6a" * 32, b"\x5b" * 32, NET.chia_toll_mojos)
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: other)

    # Keyed lookups: the nonce coin is NOT indexed yet (that is the resume
    # window where re-election was reproduced); the pinned coin is.
    pinned_record = SimpleNamespace(
        coin=SimpleNamespace(
            parent_coin_info=pinned_parent.hex(),
            puzzle_hash=(b"\x5b" * 32).hex(),
            amount=NET.chia_toll_mojos,
        ),
        spent=False, confirmed_block_index=100, spent_block_index=0,
    )
    records = {pinned.name().hex(): pinned_record}
    ctx.coinset.get_coin_record_by_name = lambda name: records.get(name)
    ctx.coinset.by_ph = [
        SimpleNamespace(
            coin=SimpleNamespace(parent_coin_info="77" * 32,
                                 puzzle_hash="bb" * 32, amount=AMOUNT),
            spent=False, confirmed_block_index=100, spent_block_index=0,
        )
    ]

    built = {}
    monkeypatch.setattr(claim_mod, "cat_lineage_proof_for",
                        lambda coinset, parent: [b"\x33" * 32, b"\x44" * 32, AMOUNT])
    def capture(req):
        built["coin"] = req.security_coin
        raise S.WarpPending("stop here")
    monkeypatch.setattr(drivers, "build_burn_bundle", capture)

    engine.step()
    # The pinned record also satisfies the burn-CAT search in this fake, so
    # the build reached capture() -- with the PINNED coin, not `other`.
    assert built["coin"].name() == pinned.name()


def test_wallet_resolution_transient_error_is_retryable():
    """[UNWRAP-RESOLVE-RETRY] One flaky RPC must not FAIL the job; absence is
    only proven when every candidate answered."""
    from gui.services.warp.wallet import WalletError

    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())

    def flaky(wallet_id):
        raise WalletError("rpc timeout")

    ctx.wallet.cat_get_asset_id = flaky
    request(engine)
    out = engine.step()

    assert out["status"] == JobStatus.UNWRAP_CHECKS, "retryable, not FAILED"
    assert out["retry_count"] == 1


def test_a_threshold_change_bounces_relay_back_to_collection(monkeypatch):
    """[UNWRAP-THRESHOLD-DRIFT] The contract checks the sig blob length
    EXACTLY; a threshold change since collection would revert every relay
    forever. Signatures never expire, so re-collecting is lossless."""
    from gui.services.warp import evm as evm_mod

    monkeypatch.setattr(evm_mod, "sign_tx", unwrap_sign_tx)
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    _seed_relaying(store)
    ctx.evm.threshold = 7                     # owner raised it after collection

    out = engine.step()

    assert out["status"] == JobStatus.COLLECTING_EVM_SIGS
    assert store.get_active_job().state.get("relay_sigs") is None


# --------------------------------------------------------------------------- #
# PR #72 review comments (Copilot), pinned.
# --------------------------------------------------------------------------- #

def test_cap_parsers_reject_junk_with_a_clear_error():
    """[PR72-REVIEW] float(raw) let 'abc' surface as a raw ValueError, 1e309
    overflow downstream, and nan slip PAST the positivity check entirely
    (NaN comparisons are always False). Both cap keys and the min share the
    parser now."""
    for key in ("max_unwrap_usdc", "max_auto_bridge_usdc"):
        for bad in ("abc", float("nan"), float("inf"), "1e309", [1]):
            cfg = {"warp": {"enabled": key == "max_auto_bridge_usdc",
                            key: bad}}
            if key == "max_auto_bridge_usdc":
                cfg["warp"]["max_auto_bridge_usdc"] = bad
            with pytest.raises(S.WarpError, match="is not a number|must be finite"):
                S.warp_params_from_config(cfg)

    with pytest.raises(S.WarpError, match="min_auto_bridge_usdc"):
        S.warp_params_from_config({"warp": {"min_auto_bridge_usdc": "abc"}})
