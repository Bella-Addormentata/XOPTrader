"""[WARP-LOST-SEND 2026-10-25] A recorded Chia send that will never land.

Job 4 on 2026-10-24: the claim's funding send spent a coin an engine offer
also used; the offer's spend landed, and the engine's stuck-transaction
cleanup deleted the warp send from the wallet.  The job waited two hours for
a coin that could never appear, then failed, and Retry had nothing to do.

These tests drive the real handlers with the recorded send judged from the
wallet and the chain: a dead send is sent again (a cat_spend only when the
chain proves it dead, otherwise on the operator's Retry), a live one is left
alone, and every send now pays a fee -- the engine's offer fee at least.
"""

from __future__ import annotations

from types import SimpleNamespace

import pytest

pytest.importorskip("clvm")
pytest.importorskip("chia_rs")

from gui.services.warp import claim as claim_mod  # noqa: E402
from gui.services.warp import service as S  # noqa: E402
from gui.services.warp.jobs import JobStatus  # noqa: E402

from .test_warp_service import (  # noqa: E402
    FakeCoin,
    build,
    default_params,
    make_ephemeral_blob,
    new_store,
    seed,
)
from .test_warp_unwrap_service import (  # noqa: E402
    AMOUNT,
    NET,
    build_unwrap,
    params_with_cap,
    request,
)

# Job 4's funding send and the 0.499 XCH coin it spent, which an offer took.
DEAD_TX = "a7f5f64aae1c886e9eb47382aebc47c790e093f385049dc0d7c9601bdd83ef70"
RID = "c4a91ced4257254eb8a1e7c166de014eb3daaedd0f8a8c0ef6f41cfc09847d3c"
SECURITY_PH = b"\x5b" * 32
EXPECTED = 4985 + 100_000_000          # post-tip + the default claim fee
LIMIT = S._ORPHAN_POLLS_BEFORE_RESEND
PEAK = 9_478_613                       # 837 blocks past the conflicting spend


def _last_message(store, job_id) -> str:
    """The newest audit event's message (the step result carries none)."""
    events = store.get_events(job_id, limit=5000)
    return events[-1].message if events else ""


def _rec(*, spent: bool):
    return SimpleNamespace(
        coin=SimpleNamespace(parent_coin_info="d4" * 32, puzzle_hash="11" * 32,
                             amount=499_005_000_000),
        spent=spent, confirmed_block_index=9_476_193,
        spent_block_index=9_477_776 if spent else 0,
    )


def _no_coin(monkeypatch):
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    monkeypatch.setattr(claim_mod, "security_coin_puzzle_hash", lambda sk: SECURITY_PH)


def _seed_funding(store, **state):
    job = seed(store, JobStatus.FUNDING_CLAIM,
               columns={"receiver_ph": "ab" * 32, "post_tip_mojos": 4985})
    blob, _sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob, **state})
    return store.get_active_job()


# --------------------------------------------------------------------------- #
# The claim's funding send.
# --------------------------------------------------------------------------- #

def test_job_4s_funding_send_is_sent_again(monkeypatch):
    """Gone from the wallet, its coin spent by another transaction, no
    output: dead, so the handler funds the security coin again at once."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=DEAD_TX, funding_amount=EXPECTED,
                  funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK

    out = engine.step()

    assert out["status"] == JobStatus.FUNDING_CLAIM
    assert [s[0] for s in ctx.wallet.sent] == [EXPECTED]
    st = store.get_active_job().state
    assert st["funding_tx_id"] == "funding-tx-1"
    assert st["funding_tx_id_prior"] == [DEAD_TX]
    assert st["funding_tx_id_orphan_polls"] == 0
    assert st["funding_amount"] == EXPECTED


def test_a_dead_send_still_in_the_history_is_not_adopted_again(monkeypatch):
    """The wallet may keep a DOUBLE_SPEND-ing record; the dedupe scan must
    skip the id already proven dead, or the job waits forever again."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=DEAD_TX, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK
    ctx.wallet.tx_records[DEAD_TX] = {"confirmed": False, "removals": []}
    ctx.wallet.txs = [{"name": DEAD_TX, "additions": [
        {"puzzle_hash": SECURITY_PH.hex(), "amount": EXPECTED}]}]

    engine.step()

    assert len(ctx.wallet.sent) == 1
    assert store.get_active_job().state["funding_tx_id"] == "funding-tx-1"


def test_a_send_the_wallet_still_holds_is_left_alone(monkeypatch):
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=DEAD_TX, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=False)
    ctx.wallet.tx_records[DEAD_TX] = {"confirmed": False, "removals": []}

    engine.step()

    assert ctx.wallet.sent == []
    job = store.get_active_job()
    assert "wallet still holds" in _last_message(store, job.id)
    assert job.state["funding_tx_id"] == DEAD_TX


def test_a_send_only_the_mempool_holds_is_left_alone_and_teaches_its_coins(
        monkeypatch):
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=DEAD_TX)       # no removals recorded
    ctx.coinset.mempool[DEAD_TX] = {"removals": [
        {"parent_coin_info": "0x" + "d4" * 32, "puzzle_hash": "0x" + "11" * 32,
         "amount": 499_005_000_000}]}

    engine.step()

    assert ctx.wallet.sent == []
    job = store.get_active_job()
    assert "mempool" in _last_message(store, job.id)
    st = job.state
    assert st["funding_tx_id_removals"] == [
        S_coin_id("d4" * 32, "11" * 32, 499_005_000_000)]


def S_coin_id(parent, ph, amount):
    from gui.services.warp import chia_send as cs

    return cs.coin_id(parent, ph, amount)


def test_an_orphaned_funding_send_is_sent_again_after_the_wait(monkeypatch):
    """Nowhere, coins unspent: it may still surface, so the handler waits
    LIMIT polls, then sends again EXCLUDING the first send's coins so the
    two can never conflict."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=DEAD_TX, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=False)

    for n in range(1, LIMIT):
        engine.step()
        assert ctx.wallet.sent == [], f"poll {n}"
        job = store.get_active_job()
        assert job.state["funding_tx_id_orphan_polls"] == n
        assert f"({n}/{LIMIT})" in _last_message(store, job.id)

    engine.step()
    assert len(ctx.wallet.sent) == 1
    assert ctx.wallet.sent_excludes == [[RID]]
    st = store.get_active_job().state
    assert st["funding_tx_id"] == "funding-tx-1"
    assert st["funding_tx_id_prior"] == [DEAD_TX]


def test_an_unnamed_send_adopts_the_id_the_history_shows(monkeypatch):
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=S._FUNDING_TX_UNKNOWN)
    ctx.wallet.txs = [{"name": "hist-1", "additions": [
        {"puzzle_hash": SECURITY_PH.hex(), "amount": EXPECTED}]}]

    engine.step()

    assert ctx.wallet.sent == []
    assert store.get_active_job().state["funding_tx_id"] == "hist-1"


def test_an_unnamed_send_the_history_never_shows_is_sent_again(monkeypatch):
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=S._FUNDING_TX_UNKNOWN)

    for _ in range(LIMIT - 1):
        engine.step()
    assert ctx.wallet.sent == []
    engine.step()
    assert len(ctx.wallet.sent) == 1
    st = store.get_active_job().state
    assert st["funding_tx_id"] == "funding-tx-1"
    assert st["funding_tx_id_prior"] == [S._FUNDING_TX_UNKNOWN]


def test_an_unanswered_probe_changes_nothing(monkeypatch):
    """The wallet did not answer: not dead, not an orphan poll."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=DEAD_TX, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=False)

    def down(tx_id):
        raise RuntimeError("wallet rpc down")

    ctx.wallet.get_transaction_or_none = down
    engine.step()
    assert ctx.wallet.sent == []
    assert not store.get_active_job().state.get("funding_tx_id_orphan_polls")


def test_a_send_waits_for_a_synced_wallet(monkeypatch):
    """Job 4's send went out while the wallet was still flapping."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store)
    ctx.wallet.synced = False

    out = engine.step()

    assert ctx.wallet.sent == []
    assert out["status"] == JobStatus.FUNDING_CLAIM
    assert out["retry_count"] == 0, "a healthy pend, not an error"
    assert "syncing" in _last_message(store, out["id"])
    ctx.wallet.synced = True
    engine.step()
    assert len(ctx.wallet.sent) == 1


def test_a_fresh_send_records_the_coins_it_spent(monkeypatch):
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store)
    ctx.wallet.next_removals = [{"parent_coin_info": "0x" + "d4" * 32,
                                 "puzzle_hash": "0x" + "11" * 32,
                                 "amount": 499_005_000_000}]
    engine.step()
    st = store.get_active_job().state
    assert st["funding_tx_id_removals"] == [
        S_coin_id("d4" * 32, "11" * 32, 499_005_000_000)]
    assert st["funding_fee_mojos"] == 0        # the harness has no fee floor


# --------------------------------------------------------------------------- #
# The unwrap's cat_spend and toll funding send.
# --------------------------------------------------------------------------- #

def _to_burn_sent(store, engine):
    request(engine)
    assert engine.step()["status"] == JobStatus.BURN_SENT
    return store.get_active_job()


def test_a_cat_spend_whose_coin_went_elsewhere_is_sent_again():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={"burn_tx_id": "burn-dead",
                                          "burn_tx_id_removals": [RID]})
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK

    engine.step()

    assert [(w, a) for w, a, _addr, _fee in ctx.wallet.cat_spends] == [(3, AMOUNT)]
    st = store.get_active_job().state
    assert st["burn_tx_id"] == "burn-tx-1"
    assert st["burn_tx_id_prior"] == ["burn-dead"]


def test_a_vanished_cat_spend_waits_for_the_operators_retry():
    """Nowhere, coins unspent: never re-sent on the handler's own authority
    (a second burn is a real loss if the first surfaces).  The operator's
    Retry clears it, and the next tick sends."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={"burn_tx_id": "burn-gone",
                                          "burn_tx_id_removals": [RID]})
    ctx.coinset.records[RID] = _rec(spent=False)

    for _ in range(LIMIT + 5):
        engine.step()
    assert ctx.wallet.cat_spends == []
    assert "Retry" in _last_message(store, job.id)

    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.BURN_SENT,
                     state_patch={"failed_from": JobStatus.BURN_SENT})
    engine.job_action(job.id, "retry")
    st = store.get_job(job.id).state
    assert st["burn_tx_id"] is None
    assert st["burn_tx_id_prior"] == ["burn-gone"]
    assert store.get_job(job.id).status == JobStatus.BURN_SENT

    engine.step()
    assert len(ctx.wallet.cat_spends) == 1


def test_retry_keeps_a_cat_spend_the_wallet_still_holds():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={"burn_tx_id": "burn-live",
                                          "burn_tx_id_removals": [RID]})
    ctx.coinset.records[RID] = _rec(spent=False)
    ctx.wallet.tx_records["burn-live"] = {"confirmed": False}
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.BURN_SENT,
                     state_patch={"failed_from": JobStatus.BURN_SENT})
    engine.job_action(job.id, "retry")
    assert store.get_job(job.id).state["burn_tx_id"] == "burn-live"
    engine.step()
    assert ctx.wallet.cat_spends == []


def test_a_toll_funding_send_whose_coin_went_elsewhere_is_sent_again(monkeypatch):
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = seed(store, JobStatus.BURNING, columns={"amount_mojos": AMOUNT},
               state={"direction": "out", "receiver_evm": "ab" * 20,
                      "burn_cat_ph": "bb" * 32, "cat_wallet_id": 3})
    blob, _sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={
        "ephemeral_blob": blob, "funding_tx_id": "toll-dead",
        "funding_amount": NET.chia_toll_mojos, "funding_tx_id_removals": [RID]})
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK

    engine.step()

    assert [s[0] for s in ctx.wallet.sent] == [NET.chia_toll_mojos]
    st = store.get_active_job().state
    assert st["funding_tx_id"] == "funding-tx-1"
    assert st["funding_tx_id_prior"] == ["toll-dead"]


# --------------------------------------------------------------------------- #
# Fees: the offer fee is the floor of every automatic Chia fee.
# --------------------------------------------------------------------------- #

def test_the_automatic_fee_floors_at_the_offer_fee_and_follows_the_node():
    store = new_store()
    engine, ctx = build(store, params=default_params(offer_fee_mojos=10_000_000))
    ctx.coinset.fee_estimates = [3_948_647]          # coinset, 2026-10-24
    assert engine._chia_send_fee(0, cost=S._COST_XCH_SEND) == 10_000_000
    ctx.coinset.fee_estimates = [25_000_000]         # the mempool asks more
    assert engine._chia_send_fee(0, cost=S._COST_XCH_SEND) == 25_000_000
    ctx.coinset.fee_estimates = [10 ** 12]           # ...but never past 10x
    assert engine._chia_send_fee(0, cost=S._COST_XCH_SEND) == 100_000_000
    assert engine._chia_send_fee(7, cost=S._COST_XCH_SEND) == 7, "a number pins it"

    def down(*, cost, target_times):
        raise RuntimeError("coinset down")

    ctx.coinset.get_fee_estimate = down
    assert engine._chia_send_fee(0, cost=S._COST_XCH_SEND) == 10_000_000


def test_the_offer_fee_floor_is_read_from_the_strategy_section():
    assert S._offer_fee_floor_mojos({"strategy": {"offer_fee_mojos": 15_000_000}}) == 15_000_000
    assert S._offer_fee_floor_mojos({}) == 10_000_000
    assert S._offer_fee_floor_mojos({"strategy": {"offer_fee_mojos": "abc"}}) == 10_000_000
    assert S._offer_fee_floor_mojos({"strategy": {"offer_fee_mojos": True}}) == 10_000_000
    assert S.warp_params_from_config(
        {"warp": {"enabled": False}, "strategy": {"offer_fee_mojos": 15_000_000}}
    ).offer_fee_mojos == 15_000_000


def test_the_funding_send_pays_the_automatic_fee(monkeypatch):
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store, params=default_params(offer_fee_mojos=10_000_000))
    _seed_funding(store)
    ctx.coinset.fee_estimates = [25_000_000]
    engine.step()
    assert ctx.wallet.sent == [(EXPECTED, ctx.wallet.sent[0][1], 25_000_000)]
    assert store.get_active_job().state["funding_fee_mojos"] == 25_000_000


def test_g10_budgets_the_automatic_fee_twice():
    toll, fee = NET.chia_toll_mojos, 10_000_000
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap(offer_fee_mojos=fee))
    ctx.wallet.balances[1] = {"confirmed_wallet_balance": toll + fee,
                              "spendable_balance": toll + fee}
    request(engine)
    assert engine.step()["status"] == JobStatus.UNWRAP_CHECKS, "one fee pends"
    ctx.wallet.balances[1] = {"confirmed_wallet_balance": toll + 2 * fee,
                              "spendable_balance": toll + 2 * fee}
    assert engine.step()["status"] == JobStatus.BURN_SENT
    engine.step()
    assert ctx.wallet.cat_spends[0][3] == fee, "the cat_spend carries the fee"


def test_the_sweep_pays_the_automatic_fee_from_the_coin(monkeypatch):
    seen = {}

    def capture(coinset, net, *, security_coin, destination_puzzle_hash,
                ephemeral_sk, sweep_fee=0):
        seen["fee"] = sweep_fee
        return ("accepted", "ok")

    monkeypatch.setattr(claim_mod, "build_and_push_sweep", capture)
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    for floor, want in ((10_000_000, 10_000_000), (10 ** 12, EXPECTED // 2)):
        store = new_store()
        engine, _ctx = build(store, params=default_params(offer_fee_mojos=floor))
        job = _seed_funding(store, funding_amount=EXPECTED)
        resolved, _status = engine._sweep_security(job, FakeCoin(b"\x5a" * 32), b"\x01" * 32)
        assert resolved and seen["fee"] == want


# --------------------------------------------------------------------------- #
# Review round 1.
# --------------------------------------------------------------------------- #

def test_the_excluded_coins_reach_the_daemon_under_its_field_name():
    """[review] The daemon's tx_config field is excluded_coin_ids."""
    from gui.services.warp.wallet import WalletClient

    seen = {}

    def poster(url, body):
        seen["body"] = body
        return {"success": True, "transaction": {"name": "0xaa"}}

    WalletClient({}, poster=poster).send_transaction(
        1, 5, "xch1abc", fee_mojos=3, exclude_coin_ids=[RID, "0x" + "ee" * 32])
    assert seen["body"]["excluded_coin_ids"] == ["0x" + RID, "0x" + "ee" * 32]
    assert "exclude_coin_ids" not in seen["body"]


def test_a_dead_cat_spends_output_pin_goes_with_it():
    """[review] The replacement must not inherit the first send's burn CAT
    coin id, or completion polls a coin that can never exist."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={
        "burn_tx_id": "burn-dead", "burn_tx_id_removals": [RID],
        "burn_cat_coin_id": "aa" * 32})
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK
    engine.step()
    st = store.get_active_job().state
    assert len(ctx.wallet.cat_spends) == 1
    assert st.get("burn_cat_coin_id") is None
    assert st["burn_tx_id"] == "burn-tx-1"


def test_retry_clears_the_output_pin_with_the_vanished_cat_spend():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={
        "burn_tx_id": "burn-gone", "burn_tx_id_removals": [RID],
        "burn_cat_coin_id": "aa" * 32})
    ctx.coinset.records[RID] = _rec(spent=False)
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.BURN_SENT,
                     state_patch={"failed_from": JobStatus.BURN_SENT})
    engine.job_action(job.id, "retry")
    st = store.get_job(job.id).state
    assert st["burn_tx_id"] is None and st["burn_cat_coin_id"] is None


def test_an_unnamed_send_never_adopts_an_id_proven_dead(monkeypatch):
    """[review] The history scan for an unnamed send skips the dead ids."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=S._FUNDING_TX_UNKNOWN,
                  funding_tx_id_prior=[DEAD_TX])
    ctx.wallet.txs = [{"name": DEAD_TX, "additions": [
        {"puzzle_hash": SECURITY_PH.hex(), "amount": EXPECTED}]}]
    engine.step()
    st = store.get_active_job().state
    assert st["funding_tx_id"] == S._FUNDING_TX_UNKNOWN
    assert st["funding_tx_id_orphan_polls"] == 1
    assert ctx.wallet.sent == []


def test_the_sweep_fee_is_fixed_on_the_first_sweep(monkeypatch):
    """[review] A re-sweep must push the SAME bundle: a differently-priced
    twin conflicts with the pending first one in the mempool."""
    fees = []

    def capture(coinset, net, *, security_coin, destination_puzzle_hash,
                ephemeral_sk, sweep_fee=0):
        fees.append(sweep_fee)
        return ("accepted", "ok")

    monkeypatch.setattr(claim_mod, "build_and_push_sweep", capture)
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: False)
    store = new_store()
    engine, ctx = build(store, params=default_params(offer_fee_mojos=10_000_000))
    job = _seed_funding(store, funding_amount=EXPECTED)
    engine._sweep_security(job, FakeCoin(b"Z" * 32), b"" * 32)
    assert store.get_active_job().state["sweep_fee_mojos"] == 10_000_000
    ctx.coinset.fee_estimates = [50_000_000]            # the market moved
    engine._sweep_security(store.get_active_job(), FakeCoin(b"Z" * 32), b"" * 32)
    assert fees == [10_000_000, 10_000_000]


def test_a_sweep_conflict_resolves_only_a_spent_coin(monkeypatch):
    """[review] A mempool conflict may be our own pending sweep; only an
    on-chain spend means nothing is left to recover."""
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: ("conflict", "MEMPOOL_CONFLICT"))
    store = new_store()
    engine, _ctx = build(store)
    job = _seed_funding(store, funding_amount=EXPECTED)
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: False)
    resolved, status = engine._sweep_security(job, FakeCoin(b"Z" * 32), b"" * 32)
    assert not resolved and "pending spend" in status
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    resolved, _status = engine._sweep_security(job, FakeCoin(b"Z" * 32), b"" * 32)
    assert resolved


# --------------------------------------------------------------------------- #
# Review round 2.
# --------------------------------------------------------------------------- #

def test_a_shallow_conflict_does_not_authorise_a_second_cat_spend():
    """[review] The conflicting spend must reach chia_confirmation_min_height
    before it counts: a reorg could return the coin and validate the first
    cat_spend, and a second one would then burn twice."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={"burn_tx_id": "burn-conflicted",
                                          "burn_tx_id_removals": [RID]})
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = 9_477_776 + 5                 # 6 confirmations
    for _ in range(3):
        engine.step()
    assert ctx.wallet.cat_spends == []
    st = store.get_active_job().state
    assert st["burn_tx_id"] == "burn-conflicted"
    assert "6/" in _last_message(store, job.id)
    assert not st.get("burn_tx_id_orphan_polls")
    ctx.coinset.peak = 9_477_776 + NET.chia_confirmation_min_height - 1
    engine.step()
    assert len(ctx.wallet.cat_spends) == 1


def test_an_unknown_peak_never_counts_a_spend_as_proof(monkeypatch):
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=DEAD_TX, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = 0
    engine.step()
    assert ctx.wallet.sent == []


def test_the_cat_spend_waits_until_the_toll_and_both_fees_are_covered():
    """[review] After the burn the job is forward-only: the toll coin and its
    fee must still be affordable on the tick that commits."""
    toll, fee = NET.chia_toll_mojos, 10_000_000
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap(offer_fee_mojos=fee))
    _to_burn_sent(store, engine)
    ctx.wallet.balances[1] = {"confirmed_wallet_balance": toll + 2 * fee - 1,
                              "spendable_balance": toll + 2 * fee - 1}
    out = engine.step()
    assert ctx.wallet.cat_spends == []
    assert out["status"] == JobStatus.BURN_SENT and out["retry_count"] == 0
    ctx.wallet.balances[1] = {"confirmed_wallet_balance": toll + 2 * fee,
                              "spendable_balance": toll + 2 * fee}
    engine.step()
    assert len(ctx.wallet.cat_spends) == 1
