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
LIMIT = 20                             # polls a test waits to prove "never"
COIN = (b"\x5a" * 32).hex()               # the FakeCoin every sweep test uses
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


def test_an_orphaned_funding_send_waits_for_retry_then_resends_excluding_its_coins(
        monkeypatch):
    """Nowhere, coins unspent: it may still surface, and a replacement
    spends other coins, so the handler never replaces it on its own.  The
    operator's Retry does, EXCLUDING the first send's coins, and the first
    send stays on the books (prior + its coins) for Sweep to account for."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_tx_id=DEAD_TX, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=False)

    for n in range(1, LIMIT + 1):
        engine.step()
        assert ctx.wallet.sent == [], f"poll {n}"
        job = store.get_active_job()
        assert job.state["funding_tx_id_orphan_polls"] == n
        assert "Retry" in _last_message(store, job.id)

    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.FUNDING_CLAIM,
                     state_patch={"failed_from": JobStatus.FUNDING_CLAIM})
    engine.job_action(job.id, "retry")
    st = store.get_job(job.id).state
    assert st["funding_tx_id"] is None
    assert st["funding_tx_id_exclude"] == [RID]
    assert st["funding_tx_id_prior_removals"] == {DEAD_TX: [RID]}

    engine.step()
    assert len(ctx.wallet.sent) == 1
    assert ctx.wallet.sent_excludes == [[RID]]
    st = store.get_active_job().state
    assert st["funding_tx_id"] == "funding-tx-1"
    assert st["funding_tx_id_prior"] == [DEAD_TX]
    assert st["funding_tx_id_exclude"] is None


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


def test_an_unnamed_send_the_history_never_shows_waits_for_retry(monkeypatch):
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_tx_id=S._FUNDING_TX_UNKNOWN)

    for _ in range(LIMIT):
        engine.step()
    assert ctx.wallet.sent == []
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.FUNDING_CLAIM,
                     state_patch={"failed_from": JobStatus.FUNDING_CLAIM})
    engine.job_action(job.id, "retry")
    assert store.get_job(job.id).state["funding_tx_id"] is None
    engine.step()
    assert len(ctx.wallet.sent) == 1
    st = store.get_active_job().state
    assert st["funding_tx_id"] == "funding-tx-1"
    assert st["funding_tx_id_prior"] == [S._FUNDING_TX_UNKNOWN + "#1"]


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


def test_a_vanished_cat_spend_is_never_replaced_until_proven_dead():
    """Nowhere, coins unspent: a replacement would spend other coins, so
    both could land -- a second CAT on the forward-only burn puzzle.  Not
    the handler, and not Retry, replaces it; the moment anything spends its
    coins (the chain proves it dead) the handler does."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={"burn_tx_id": "burn-gone",
                                          "burn_tx_id_removals": [RID]})
    ctx.coinset.records[RID] = _rec(spent=False)

    for _ in range(LIMIT + 5):
        engine.step()
    assert ctx.wallet.cat_spends == []
    assert "proves it dead" in _last_message(store, job.id)

    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.BURN_SENT,
                     state_patch={"failed_from": JobStatus.BURN_SENT})
    engine.job_action(job.id, "retry")
    st = store.get_job(job.id).state
    assert st["burn_tx_id"] == "burn-gone", "Retry leaves an orphaned burn alone"
    assert store.get_job(job.id).status == JobStatus.BURN_SENT
    engine.step()
    assert ctx.wallet.cat_spends == []

    ctx.coinset.records[RID] = _rec(spent=True)             # its coins went elsewhere
    ctx.coinset.peak = PEAK
    engine.step()
    assert len(ctx.wallet.cat_spends) == 1
    st = store.get_active_job().state
    assert st["burn_tx_id"] == "burn-tx-1"
    assert st["burn_tx_id_prior"] == ["burn-gone"]


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


def test_retry_clears_the_output_pin_with_a_dead_cat_spend():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={
        "burn_tx_id": "burn-dead", "burn_tx_id_removals": [RID],
        "burn_cat_coin_id": "aa" * 32})
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK
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
    assert store.get_active_job().state["sweep_fee_by_coin"] == {COIN: 10_000_000}
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


# --------------------------------------------------------------------------- #
# Review round 3.
# --------------------------------------------------------------------------- #

def test_the_marker_is_reserved_before_the_wallet_rpc(monkeypatch):
    """[review] The engine's guard reads the marker; it must exist while
    the send is being made, not only once the daemon has answered."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store)
    seen = {}
    real = ctx.wallet.send_transaction

    def observe(*a, **k):
        seen["during"] = store.get_job(job.id).state.get("funding_tx_id")
        return real(*a, **k)

    ctx.wallet.send_transaction = observe
    engine.step()
    assert seen["during"] == S._FUNDING_TX_UNKNOWN
    assert store.get_active_job().state["funding_tx_id"] == "funding-tx-1"


def test_a_daemon_refusal_releases_the_reservation(monkeypatch):
    from gui.services.warp.wallet import WalletError

    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store)
    calls = []

    def refuse(*a, **k):
        calls.append(1)
        if len(calls) == 1:
            raise WalletError("send_transaction failed: insufficient funds")
        return {"name": "funding-tx-2"}

    ctx.wallet.send_transaction = refuse
    out = engine.step()
    assert out["retry_count"] == 1
    assert store.get_job(job.id).state.get("funding_tx_id") is None
    store.update_job(job.id, columns={"next_retry_at": None})
    engine.step()
    assert store.get_active_job().state["funding_tx_id"] == "funding-tx-2"


def test_a_transport_failure_keeps_the_reservation_for_the_history_scan(
        monkeypatch):
    """[review] A timed-out RPC may have broadcast: the reservation stands,
    and the next tick adopts the id the wallet history shows."""
    from gui.services.warp.wallet import WalletError

    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store)

    def timeout(*a, **k):
        raise WalletError("send_transaction request failed: timed out")

    ctx.wallet.send_transaction = timeout
    engine.step()
    assert store.get_job(job.id).state["funding_tx_id"] == S._FUNDING_TX_UNKNOWN
    ctx.wallet.txs = [{"name": "broadcast-after-all", "additions": [
        {"puzzle_hash": SECURITY_PH.hex(), "amount": EXPECTED}]}]
    store.update_job(job.id, columns={"next_retry_at": None})
    engine.step()
    assert store.get_active_job().state["funding_tx_id"] == "broadcast-after-all"


def test_g10_budgets_each_spend_at_its_own_cost():
    """[review] toll + the cat_spend's fee + the XCH send's fee, each from
    the node's estimate for THAT spend's cost."""
    toll = NET.chia_toll_mojos
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap(offer_fee_mojos=10_000_000))
    ctx.coinset.fee_estimates_by_cost = {S._COST_CAT_SPEND: [40_000_000],
                                         S._COST_XCH_SEND: [12_000_000]}
    assert engine._unwrap_xch_budget() == toll + 40_000_000 + 12_000_000


def test_the_jobs_db_path_expands_env_refs_and_tilde_like_the_engine(monkeypatch):
    monkeypatch.setenv("XOP_JOBS_DIR", "/srv/xop")
    assert S._job_db_path({"warp": {"jobs_db": "${XOP_JOBS_DIR}/jobs.db"}},
                          anchor="/cfg").replace("\\", "/") == "/srv/xop/jobs.db"
    assert S._job_db_path({"warp": {"jobs_db": "${NOPE_UNSET:-data}/j.db"}},
                          anchor="/cfg").replace("\\", "/") == "/cfg/data/j.db"
    monkeypatch.setenv("HOME", "/home/op")
    monkeypatch.setenv("USERPROFILE", "/home/op")
    assert S._job_db_path({"warp": {"jobs_db": "~/warp_jobs.db"}},
                          anchor="/cfg").replace("\\", "/") == "/home/op/warp_jobs.db"
    with pytest.raises(S.WarpError):
        S._job_db_path({"warp": {"jobs_db": "${NOPE_UNSET}/j.db"}}, anchor="/cfg")


# --------------------------------------------------------------------------- #
# Review round 4.
# --------------------------------------------------------------------------- #

def test_a_refused_replacement_restores_the_marker_and_keeps_its_exclusions(
        monkeypatch):
    """[review] A refused replacement must not lose what Retry decided: the
    exclusions persist in state, so the next attempt still skips the
    orphan's coins, and a refused DEAD replacement is judged again."""
    from gui.services.warp.wallet import WalletError

    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_tx_id=DEAD_TX, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=False)          # an orphan
    engine.step()
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.FUNDING_CLAIM,
                     state_patch={"failed_from": JobStatus.FUNDING_CLAIM})
    engine.job_action(job.id, "retry")                     # cleared, exclude [RID]
    calls = []
    real = ctx.wallet.send_transaction

    def refuse_once(*a, **k):
        calls.append(k.get("exclude_coin_ids"))
        if len(calls) == 1:
            raise WalletError("send_transaction failed: insufficient funds")
        return real(*a, **k)

    ctx.wallet.send_transaction = refuse_once
    out = engine.step()
    assert out["retry_count"] == 1
    st = store.get_job(job.id).state
    assert st["funding_tx_id"] is None, "as Retry left it"
    assert st["funding_tx_id_exclude"] == [RID], "the exclusions survive"
    store.update_job(job.id, columns={"next_retry_at": None})
    engine.step()
    assert calls == [[RID], [RID]], "both attempts excluded the orphan's coin"
    st = store.get_active_job().state
    assert st["funding_tx_id"] == "funding-tx-1"
    assert st["funding_tx_id_prior"] == [DEAD_TX]


def test_a_refused_dead_replacement_is_judged_again(monkeypatch):
    from gui.services.warp.wallet import WalletError

    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_tx_id=DEAD_TX, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK
    calls = []
    real = ctx.wallet.send_transaction

    def refuse_once(*a, **k):
        calls.append(1)
        if len(calls) == 1:
            raise WalletError("send_transaction failed: insufficient funds")
        return real(*a, **k)

    ctx.wallet.send_transaction = refuse_once
    engine.step()
    st = store.get_job(job.id).state
    assert st["funding_tx_id"] == DEAD_TX, "the marker the tick started with"
    assert not st.get("funding_tx_id_prior")
    store.update_job(job.id, columns={"next_retry_at": None})
    engine.step()
    st = store.get_active_job().state
    assert st["funding_tx_id"] == "funding-tx-1"
    assert st["funding_tx_id_prior"] == [DEAD_TX]
    assert st["funding_tx_id_prior_removals"] == {DEAD_TX: [RID]}


def test_a_replacement_cat_spend_that_times_out_keeps_its_metadata():
    """[review] The dead id, the cleared pin and removals are persisted WITH
    the reservation, so an uncertain broadcast leaves a state the unnamed-
    send path can finish from."""
    from gui.services.warp.wallet import WalletError

    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={
        "burn_tx_id": "burn-dead", "burn_tx_id_removals": [RID],
        "burn_cat_coin_id": "aa" * 32})
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK

    def timeout(*a, **k):
        raise WalletError("cat_spend request failed: timed out")

    ctx.wallet.cat_spend = timeout
    engine.step()
    st = store.get_job(job.id).state
    assert st["burn_tx_id"] == S._FUNDING_TX_UNKNOWN
    assert st["burn_tx_id_prior"] == ["burn-dead"]
    assert st["burn_cat_coin_id"] is None
    assert st["burn_tx_id_removals"] is None
    # The wallet history then shows the replacement: adopted, pin still clear.
    burn_ph = st["burn_cat_ph"]
    ctx.wallet.cat_txs = [{"name": "burn-replacement", "additions": [
        {"puzzle_hash": burn_ph, "amount": AMOUNT,
         "parent_coin_info": "77" * 32}]}]
    store.update_job(job.id, columns={"next_retry_at": None})
    engine.step()
    st = store.get_job(job.id).state
    assert st["burn_tx_id"] == "burn-replacement"
    assert st["burn_tx_id_prior"] == ["burn-dead"]


# --------------------------------------------------------------------------- #
# Review round 5.
# --------------------------------------------------------------------------- #

def test_the_funding_scan_reads_the_unconfirmed_set_before_the_window(monkeypatch):
    """[review] The daemon sorts unconfirmed records last, so a busy wallet
    pages a just-sent funding out of the 200-row window; a miss would be
    counted toward sending again."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=S._FUNDING_TX_UNKNOWN)
    pending = {"name": "hidden-pending", "confirmed": False, "additions": [
        {"puzzle_hash": SECURITY_PH.hex(), "amount": EXPECTED}]}

    def history(wallet_id, start=0, end=200, reverse=True, confirmed=None):
        # Only the explicit unconfirmed query shows it; the window does not.
        return [pending] if confirmed is False else []

    ctx.wallet.get_transactions = history
    engine.step()
    assert ctx.wallet.sent == []
    assert store.get_active_job().state["funding_tx_id"] == "hidden-pending"

    # The same for the dedupe before a first send.
    store2 = new_store()
    engine2, ctx2 = build(store2)
    _seed_funding(store2)
    ctx2.wallet.get_transactions = history
    engine2.step()
    assert ctx2.wallet.sent == []
    assert store2.get_active_job().state["funding_tx_id"] == "hidden-pending"


# --------------------------------------------------------------------------- #
# Review round 6.
# --------------------------------------------------------------------------- #

def test_the_configured_key_is_selected_before_the_evidence_is_read(monkeypatch):
    """[review] Another client may have switched the daemon's key; a login
    failure propagates instead of reading as absence."""
    from gui.services.warp.wallet import WalletError

    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_tx_id=DEAD_TX, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=False)

    def refused():
        raise WalletError("log_in failed: fingerprint not found")

    ctx.wallet.log_in = refused
    out = engine.step()
    assert out["retry_count"] == 1
    assert ctx.wallet.sent == []
    st = store.get_job(job.id).state
    assert st["funding_tx_id"] == DEAD_TX
    assert not st.get("funding_tx_id_orphan_polls")


def test_the_sync_status_is_read_for_the_configured_key(monkeypatch):
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store)

    def sync_status():
        assert ctx.wallet.logged_in >= 1, "log_in must precede get_sync_status"
        return {"synced": True, "syncing": False}

    ctx.wallet.get_sync_status = sync_status
    engine.step()
    assert len(ctx.wallet.sent) == 1


def test_the_funded_amount_is_reserved_with_the_marker(monkeypatch):
    """[review] A timed-out funding RPC must leave the amount Sweep looks
    for, not depend on the fee configuration of the day."""
    from gui.services.warp.wallet import WalletError

    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store)

    def timeout(*a, **k):
        raise WalletError("send_transaction request failed: timed out")

    ctx.wallet.send_transaction = timeout
    engine.step()
    st = store.get_job(job.id).state
    assert st["funding_tx_id"] == S._FUNDING_TX_UNKNOWN
    assert st["funding_amount"] == EXPECTED


def test_a_definitively_rejected_sweep_fee_is_repriced_unless_already_pushed(
        monkeypatch):
    from gui.services.warp.coinset import CoinsetError

    fees = []
    outcome = {"raise": True}

    def push(coinset, net, *, security_coin, destination_puzzle_hash,
             ephemeral_sk, sweep_fee=0):
        fees.append(sweep_fee)
        if outcome["raise"]:
            raise CoinsetError("push_tx failed: [INVALID_FEE_LOW_FEE] Failed to "
                               "include transaction, error INVALID_FEE_LOW_FEE")
        return ("accepted", "ok")

    monkeypatch.setattr(claim_mod, "build_and_push_sweep", push)
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: False)
    store = new_store()
    engine, ctx = build(store, params=default_params(offer_fee_mojos=10_000_000))
    job = _seed_funding(store, funding_amount=EXPECTED)
    resolved, status = engine._sweep_security(job, FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert not resolved and "repriced" in status
    assert store.get_job(job.id).state["sweep_fee_by_coin"] == {}
    ctx.coinset.fee_estimates = [30_000_000]              # the market asks more
    outcome["raise"] = False
    engine._sweep_security(store.get_job(job.id), FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert fees == [10_000_000, 30_000_000]
    assert store.get_job(job.id).state["sweep_pushed_by_coin"] == {COIN: True}
    # Once pushed, a later fee rejection keeps the pin: a repriced twin
    # would only conflict with the bundle a node already holds.
    outcome["raise"] = True
    ctx.coinset.fee_estimates = [50_000_000]
    engine._sweep_security(store.get_job(job.id), FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert fees[-1] == 30_000_000
    assert store.get_job(job.id).state["sweep_fee_by_coin"] == {COIN: 30_000_000}


def _failed_with_replaced_funding(store, ctx, *, prior_removals):
    """A FAILED inbound job whose funding was replaced once.  The CURRENT
    attempt (replacement-tx, coins [RID2]) is settled by default: its coin
    is spent at depth and no output of its own is at the key, so the chain
    proves it dead; tests that want it pending override RID2."""
    job = seed(store, JobStatus.FAILED,
               columns={"receiver_ph": "ab" * 32, "post_tip_mojos": 4985},
               state={"failed_from": JobStatus.FUNDING_CLAIM})
    blob, _sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={
        "ephemeral_blob": blob, "funding_amount": EXPECTED,
        "funding_tx_id": "replacement-tx", "funding_tx_id_prior": [DEAD_TX],
        "funding_tx_id_prior_removals": prior_removals,
        "funding_tx_id_removals": [RID2]})
    ctx.coinset.records[RID2] = _rec(spent=True)
    ctx.coinset.peak = PEAK
    return store.get_active_job()


def test_sweep_does_not_close_while_a_replaced_funding_send_could_land(monkeypatch):
    """[review] A replacement spends other coins, so both sends can land;
    the job closes only once every replaced send is proven dead."""
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: ("accepted", "ok"))
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    store = new_store()
    engine, ctx = build(store)
    job = _failed_with_replaced_funding(store, ctx, prior_removals={DEAD_TX: [RID]})
    ctx.coinset.records[RID] = _rec(spent=False)           # the orphan could land
    out = engine.job_action(job.id, "sweep")
    assert out["status"] == JobStatus.FAILED, "left open"
    assert "could still land" in store.get_job(job.id).state["sweep_status"]

    ctx.coinset.records[RID] = _rec(spent=True)            # ...until proven dead
    ctx.coinset.peak = PEAK
    out = engine.job_action(job.id, "sweep")
    assert out["status"] == JobStatus.CANCELLED
    assert store.get_active_job() is None


def test_an_unspent_coin_from_a_replaced_send_asks_for_another_sweep(monkeypatch):
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: ("accepted", "ok"))
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    store = new_store()
    engine, ctx = build(store)
    job = _failed_with_replaced_funding(store, ctx, prior_removals={DEAD_TX: [RID]})
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK
    # The replaced send DID land: an unspent coin of the funded amount sits
    # at the ephemeral key beside the one just swept.
    ctx.coinset.by_ph = [SimpleNamespace(
        coin=SimpleNamespace(parent_coin_info="d4" * 32, puzzle_hash="5b" * 32,
                             amount=EXPECTED),
        spent=False, confirmed_block_index=PEAK - 3, spent_block_index=0)]
    out = engine.job_action(job.id, "sweep")
    assert out["status"] == JobStatus.FAILED
    assert "Sweep again" in store.get_job(job.id).state["sweep_status"]


# --------------------------------------------------------------------------- #
# Review round 7.
# --------------------------------------------------------------------------- #

def test_sweep_state_is_scoped_per_security_coin(monkeypatch):
    """[review] Two coins at the key (a replaced send landed too): the first
    coin's accepted push must not pin the second coin's rejected fee."""
    from gui.services.warp.coinset import CoinsetError

    fees = []
    rejected = {"on": False}

    def push(coinset, net, *, security_coin, destination_puzzle_hash,
             ephemeral_sk, sweep_fee=0):
        fees.append((security_coin.name().hex()[:4], sweep_fee))
        if rejected["on"]:
            raise CoinsetError("push_tx failed: [INVALID_FEE_LOW_FEE] too low")
        return ("accepted", "ok")

    monkeypatch.setattr(claim_mod, "build_and_push_sweep", push)
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: False)
    store = new_store()
    engine, ctx = build(store, params=default_params(offer_fee_mojos=10_000_000))
    job = _seed_funding(store, funding_amount=EXPECTED)
    first, second = FakeCoin(b"\x5a" * 32), FakeCoin(b"\x5b" * 32)
    engine._sweep_security(job, first, b"\x01" * 32)               # accepted
    rejected["on"] = True
    engine._sweep_security(store.get_job(job.id), second, b"\x01" * 32)
    st = store.get_job(job.id).state
    assert st["sweep_pushed_by_coin"] == {first.name().hex(): True}
    assert second.name().hex() not in st["sweep_fee_by_coin"], "released"
    assert st["sweep_fee_by_coin"][first.name().hex()] == 10_000_000, "kept"
    rejected["on"] = False
    ctx.coinset.fee_estimates = [40_000_000]
    engine._sweep_security(store.get_job(job.id), second, b"\x01" * 32)
    assert fees == [("5a5a", 10_000_000), ("5b5b", 10_000_000), ("5b5b", 40_000_000)]


def test_cat_spend_exclusions_reach_the_daemon_under_its_field_name():
    from gui.services.warp.wallet import WalletClient

    seen = {}

    def poster(url, body):
        seen["body"] = body
        return {"success": True, "transaction": {"name": "0xbb"}}

    WalletClient({}, poster=poster).cat_spend(
        3, AMOUNT, "xch1burn", fee_mojos=5, exclude_coin_ids=[RID])
    assert seen["body"]["excluded_coin_ids"] == ["0x" + RID]
    WalletClient({}, poster=poster).cat_spend(3, AMOUNT, "xch1burn")
    assert "excluded_coin_ids" not in seen["body"]


def test_retrys_burn_replacement_excludes_the_dead_sends_coins():
    """[review] As the funding paths do: the replacement names the replaced
    send's coins as exclusions and keeps them for the accounting."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={"burn_tx_id": "burn-dead",
                                          "burn_tx_id_removals": [RID]})
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.BURN_SENT,
                     state_patch={"failed_from": JobStatus.BURN_SENT})
    engine.job_action(job.id, "retry")
    assert store.get_job(job.id).state["burn_tx_id_exclude"] == [RID]
    engine.step()
    assert ctx.wallet.cat_spend_excludes == [[RID]]
    st = store.get_active_job().state
    assert st["burn_tx_id"] == "burn-tx-1"
    assert st["burn_tx_id_exclude"] is None
    assert st["burn_tx_id_prior_removals"] == {"burn-dead": [RID]}


def test_a_swept_replacements_output_does_not_settle_an_orphan(monkeypatch):
    """[review] The key holds the replacement's (swept) coin; it must not
    make the orphan read as landed while the orphan's own coins are unspent."""
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: ("accepted", "ok"))
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    store = new_store()
    engine, ctx = build(store)
    job = _failed_with_replaced_funding(store, ctx, prior_removals={DEAD_TX: [RID]})
    ctx.coinset.records[RID] = _rec(spent=False)           # the orphan lives on
    ctx.coinset.by_ph = [SimpleNamespace(                  # the replacement's coin, swept
        coin=SimpleNamespace(parent_coin_info="d4" * 32, puzzle_hash="5b" * 32,
                             amount=EXPECTED),
        spent=True, confirmed_block_index=PEAK - 3, spent_block_index=PEAK - 1)]
    out = engine.job_action(job.id, "sweep")
    assert out["status"] == JobStatus.FAILED, "left open"
    assert "could still land" in store.get_job(job.id).state["sweep_status"]


def _claiming_with_replaced_funding(store, ctx, *, prior_removals, final_hex):
    job = seed(store, JobStatus.CLAIMING,
               columns={"receiver_ph": "ab" * 32, "post_tip_mojos": 4985,
                        "bridge_nonce": "cd" * 32},
               state={"message_destination": "ee" * 32})
    blob, _sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={
        "ephemeral_blob": blob, "funding_amount": EXPECTED,
        "funding_tx_id": "replacement-tx", "funding_tx_id_prior": [DEAD_TX],
        "funding_tx_id_prior_removals": prior_removals,
        "funding_tx_id_removals": [RID2],
        "final_cat_coin_id": final_hex, "security_coin_id": "77" * 32})
    ctx.coinset.records[final_hex] = SimpleNamespace(
        coin=SimpleNamespace(parent_coin_info="99" * 32, puzzle_hash="88" * 32,
                             amount=4985),
        spent=False, confirmed_block_index=PEAK - 100, spent_block_index=0)
    # The current attempt landed and the claim spent its coin: on chain,
    # with its parent among the coins that attempt spent, and spent.
    ctx.coinset.records[RID2] = _rec(spent=True)
    ctx.coinset.by_ph = [SimpleNamespace(
        coin=SimpleNamespace(parent_coin_info=RID2, puzzle_hash="5b" * 32,
                             amount=EXPECTED),
        spent=True, confirmed_block_index=PEAK - 50, spent_block_index=PEAK - 40)]
    ctx.coinset.peak = PEAK
    return store.get_active_job()


def test_a_self_claim_completes_only_once_replaced_sends_are_ruled_out(monkeypatch):
    """[review] COMPLETED offers no Sweep: a confirmed self-claim waits in
    CLAIMING while a replaced funding send could still land."""
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    store = new_store()
    engine, ctx = build(store)
    job = _claiming_with_replaced_funding(store, ctx, prior_removals={DEAD_TX: [RID]},
                                          final_hex="f1" * 32)
    ctx.coinset.records[RID] = _rec(spent=False)           # the orphan could land
    out = engine.step()
    assert out["status"] == JobStatus.CLAIMING
    assert "could still land" in _last_message(store, job.id)
    ctx.coinset.records[RID] = _rec(spent=True)            # ...until proven dead
    out = engine.step()
    assert out["status"] == JobStatus.COMPLETED


def test_a_self_claim_sweeps_a_replaced_send_that_landed(monkeypatch):
    swept = []
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: swept.append(1) or ("accepted", "ok"))
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    store = new_store()
    engine, ctx = build(store)
    job = _claiming_with_replaced_funding(store, ctx, prior_removals={DEAD_TX: [RID]},
                                          final_hex="f2" * 32)
    ctx.coinset.records[RID] = _rec(spent=True)            # the orphan DID land:
    orphan_coin = SimpleNamespace(                         # its coin sits unspent
        coin=SimpleNamespace(parent_coin_info=RID, puzzle_hash="5b" * 32,
                             amount=EXPECTED),
        spent=False, confirmed_block_index=PEAK - 3, spent_block_index=0)
    ctx.coinset.by_ph.append(orphan_coin)
    out = engine.step()
    assert out["status"] == JobStatus.CLAIMING and swept == [1]
    assert "landed too" in _last_message(store, job.id)
    orphan_coin.spent = True                               # the sweep mined...
    orphan_coin.spent_block_index = PEAK - 40              # ...at depth
    ctx.coinset.peak = PEAK
    out = engine.step()
    assert out["status"] == JobStatus.COMPLETED


# --------------------------------------------------------------------------- #
# Review round 8.
# --------------------------------------------------------------------------- #

RID2 = "e" * 64


def test_sweep_waits_for_the_current_attempt_too(monkeypatch):
    """[review] A replaced send landed first and was swept; the CURRENT
    attempt is still pending and must keep the job open."""
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: ("accepted", "ok"))
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    store = new_store()
    engine, ctx = build(store)
    job = _failed_with_replaced_funding(store, ctx, prior_removals={DEAD_TX: [RID]})
    store.update_job(job.id, state_patch={"funding_tx_id_removals": [RID2]})
    ctx.coinset.records[RID] = _rec(spent=True)             # the prior is dead
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID2] = _rec(spent=False)           # the current one lives
    ctx.wallet.tx_records["replacement-tx"] = {"confirmed": False}
    out = engine.job_action(job.id, "sweep")
    assert out["status"] == JobStatus.FAILED, "left open"
    assert "replacement-tx"[:16] in store.get_job(job.id).state["sweep_status"]

    ctx.coinset.records[RID2] = _rec(spent=True)            # ...until it is dead too
    del ctx.wallet.tx_records["replacement-tx"]
    out = engine.job_action(job.id, "sweep")
    assert out["status"] == JobStatus.CANCELLED


def test_a_wallet_only_confirmation_does_not_settle_a_replaced_send(monkeypatch):
    """[review] The wallet calls it confirmed but the chain shows no output
    of it yet: its coin may still be unswept; keep waiting."""
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: ("accepted", "ok"))
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    store = new_store()
    engine, ctx = build(store)
    job = _failed_with_replaced_funding(store, ctx, prior_removals={DEAD_TX: [RID]})
    store.update_job(job.id, state_patch={"funding_tx_id_removals": [RID2]})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID2] = _rec(spent=True)            # the current one is dead
    ctx.coinset.records[RID] = _rec(spent=True)             # the prior spent its coin...
    ctx.wallet.tx_records[DEAD_TX] = {"confirmed": True}     # ...the wallet says confirmed
    out = engine.job_action(job.id, "sweep")                 # ...but no output on chain
    assert out["status"] == JobStatus.FAILED
    assert "could still land" in store.get_job(job.id).state["sweep_status"]


def test_exclusions_accumulate_across_replacements():
    """[review] A replaced by B, B replaced by C: C must exclude the coins
    of A as well as B."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={
        "burn_tx_id": "burn-b", "burn_tx_id_removals": [RID2],
        "burn_tx_id_prior": ["burn-a"],
        "burn_tx_id_prior_removals": {"burn-a": [RID]}})
    ctx.coinset.records[RID] = _rec(spent=True)              # A: dead
    ctx.coinset.records[RID2] = _rec(spent=True)             # B: dead
    ctx.coinset.peak = PEAK
    engine.step()                                            # B replaced by C
    st = store.get_active_job().state
    assert st["burn_tx_id"] == "burn-tx-1"
    assert st["burn_tx_id_prior_removals"] == {"burn-a": [RID], "burn-b": [RID2]}
    # ...and when C vanishes AND dies, D excludes the coins of A, B and C.
    store.update_job(job.id, state_patch={"burn_tx_id": "burn-c",
                                          "burn_tx_id_removals": ["f" * 64]})
    ctx.coinset.records["f" * 64] = _rec(spent=True)
    ctx.wallet.cat_spend_excludes.clear()
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.BURN_SENT,
                     state_patch={"failed_from": JobStatus.BURN_SENT})
    engine.job_action(job.id, "retry")
    st = store.get_job(job.id).state
    assert st["burn_tx_id_exclude"] == sorted([RID, RID2, "f" * 64])
    assert st["burn_tx_id_prior"] == ["burn-a", "burn-b", "burn-c"]
    engine.step()
    assert ctx.wallet.cat_spend_excludes == [sorted([RID, RID2, "f" * 64])]


def test_retry_of_a_burning_job_that_never_made_its_toll_key_resets_nothing():
    """[review] A BURNING job can fail generating or protecting its toll key:
    no blob, no marker.  Retry must not try to load the missing key."""
    store = new_store()
    engine, _ctx = build_unwrap(store, params=params_with_cap())
    job = seed(store, JobStatus.FAILED, columns={"amount_mojos": AMOUNT},
               state={"direction": "out", "failed_from": JobStatus.BURNING,
                      "receiver_evm": "ab" * 20, "burn_cat_ph": "bb" * 32,
                      "cat_wallet_id": 3})
    out = engine.job_action(job.id, "retry")
    assert out["status"] == JobStatus.BURNING
    assert store.get_job(job.id).state.get("funding_tx_id") is None


# --------------------------------------------------------------------------- #
# Review round 9.
# --------------------------------------------------------------------------- #

def test_a_top_up_is_not_cancelled_while_an_earlier_cat_spend_could_land():
    """[review] CANCELLED is closed with no recovery: while a replaced
    cat_spend could still land at the burn puzzle, the job waits."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={
        "rebalance_eth_target_wei": 10 ** 16, "rebalance": True,
        "burn_tx_id_prior": ["burn-a"],
        "burn_tx_id_prior_removals": {"burn-a": [RID]}})
    ctx.evm.get_erc20_balance = lambda token, owner: 0
    ctx.evm.eth = 10 ** 16                                  # target met: would cancel
    ctx.coinset.records[RID] = _rec(spent=False)             # burn-a could still land
    out = engine.step()
    assert out["status"] == JobStatus.BURN_SENT
    assert ctx.wallet.cat_spends == []
    assert "could still land" in _last_message(store, job.id)
    ctx.coinset.records[RID] = _rec(spent=True)              # ...until proven dead
    ctx.coinset.peak = PEAK
    out = engine.step()
    assert out["status"] == JobStatus.CANCELLED


def test_a_third_party_claim_completes_once_replaced_sends_are_ruled_out(monkeypatch):
    """[review] After the sweep resolved with a replaced send still pending,
    later ticks find no security coin; they must resume from the persisted
    sweep, not pend on the missing coin."""
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    store = new_store()
    engine, ctx = build(store)
    job = _claiming_with_replaced_funding(store, ctx, prior_removals={DEAD_TX: [RID]},
                                          final_hex="f3" * 32)
    del ctx.coinset.records["f3" * 32]                      # no self-claim landed
    store.update_job(job.id, state_patch={
        "final_cat_coin_id": None, "third_party_claim": True, "swept": True,
        "sweep_status": "ok"})
    ctx.coinset.records[RID] = _rec(spent=False)            # the orphan could land
    for _ in range(3):
        out = engine.step()
        assert out["status"] == JobStatus.CLAIMING
        assert out["retry_count"] == 0
    assert "could still land" in _last_message(store, job.id)
    ctx.coinset.records[RID] = _rec(spent=True)             # ...until proven dead
    out = engine.step()
    assert out["status"] == JobStatus.COMPLETED


def test_sweep_resolves_when_every_attempt_is_dead_and_nothing_was_funded(monkeypatch):
    """[review] Both the original and its replacement proven dead, no coin
    at the key: resolved, nothing to sweep, the slot is freed."""
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    store = new_store()
    engine, ctx = build(store)
    job = _failed_with_replaced_funding(store, ctx, prior_removals={DEAD_TX: [RID]})
    ctx.coinset.records[RID] = _rec(spent=True)              # the fixture made RID2 dead
    out = engine.job_action(job.id, "sweep")
    assert out["status"] == JobStatus.CANCELLED
    assert store.get_active_job() is None
    assert "nothing to sweep" in store.get_job(job.id).state["sweep_status"]


# --------------------------------------------------------------------------- #
# Review round 10.
# --------------------------------------------------------------------------- #

def test_a_landed_prior_burn_is_found_despite_a_dead_replacements_pin():
    """[review] B (pinned to a coin that never appeared) is dead; A, which
    B replaced, landed: its burn CAT carries the unwrap forward, re-pinned,
    instead of being treated as settled and cancelled over."""
    from gui.services.warp import clvm_utils as cu

    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    burn_ph = store.get_active_job().state["burn_cat_ph"]
    store.update_job(job.id, state_patch={
        "burn_tx_id": "burn-b", "burn_tx_id_removals": [RID2],
        "burn_cat_coin_id": "aa" * 32,                       # never appeared
        "burn_tx_id_prior": ["burn-a"],
        "burn_tx_id_prior_removals": {"burn-a": [RID]},
        "rebalance_eth_target_wei": 10 ** 16, "rebalance": True})
    ctx.coinset.records[RID] = _rec(spent=True)              # A landed: its coin...
    ctx.coinset.records[RID2] = _rec(spent=True)             # B dead
    ctx.coinset.peak = PEAK
    ctx.coinset.by_ph = [SimpleNamespace(                    # ...is at the burn puzzle
        coin=SimpleNamespace(parent_coin_info=RID, puzzle_hash=burn_ph, amount=AMOUNT),
        spent=False, confirmed_block_index=PEAK - 3, spent_block_index=0)]
    ctx.evm.get_erc20_balance = lambda token, owner: 0
    ctx.evm.eth = 10 ** 16                                   # would otherwise cancel
    out = engine.step()
    assert out["status"] == JobStatus.BURNING
    st = store.get_active_job().state
    assert st["burn_cat_coin_id"] == cu.coin_name(
        bytes.fromhex(RID), bytes.fromhex(burn_ph), AMOUNT).hex()
    assert st["burn_cat_parent"] == RID
    assert ctx.wallet.cat_spends == []


def test_a_malformed_wallet_reply_keeps_the_reservation(monkeypatch):
    """[review] Only the daemon's own rejection proves nothing was sent; an
    unreadable reply may follow a broadcast."""
    from gui.services.warp.wallet import WalletError

    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store)

    def garbage(*a, **k):
        raise WalletError("send_transaction: unexpected response list")

    ctx.wallet.send_transaction = garbage
    engine.step()
    assert store.get_job(job.id).state["funding_tx_id"] == S._FUNDING_TX_UNKNOWN


def test_an_unwrap_completes_only_once_replaced_toll_sends_are_ruled_out(monkeypatch):
    """[review] A replaced toll send that could still land keeps the job
    open; one that landed is swept first; then COMPLETED."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = seed(store, JobStatus.RELAYING, columns={"amount_mojos": AMOUNT},
               state={"direction": "out", "receiver_evm": "ab" * 20,
                      "post_tip_base_units": 4_985_000})
    blob, _sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={
        "ephemeral_blob": blob, "funding_amount": NET.chia_toll_mojos,
        "funding_tx_id": "toll-b", "funding_tx_id_removals": [RID2],
        "funding_tx_id_prior": ["toll-a"],
        "funding_tx_id_prior_removals": {"toll-a": [RID]}})
    job = store.get_active_job()
    ctx.coinset.records[RID2] = _rec(spent=True)             # B: settled (dead)
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=False)             # A could still land
    step = engine._unwrap_delivered(job, by="our relay", tx_hash="0x" + "77" * 32)
    assert step.next_status is None and "could still land" in step.message

    swept = []
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: swept.append(1) or ("accepted", "ok"))
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    ctx.coinset.records[RID] = _rec(spent=True)              # A landed...
    ctx.coinset.by_ph = [SimpleNamespace(                    # ...its toll coin unspent
        coin=SimpleNamespace(parent_coin_info=RID, puzzle_hash="5b" * 32,
                             amount=NET.chia_toll_mojos),
        spent=False, confirmed_block_index=PEAK - 3, spent_block_index=0)]
    step = engine._unwrap_delivered(job, by="our relay")
    assert step.next_status is None and swept == [1]

    ctx.coinset.by_ph[0].spent = True                        # the sweep mined...
    ctx.coinset.by_ph[0].spent_block_index = PEAK - 40       # ...at depth
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    step = engine._unwrap_delivered(job, by="our relay")
    assert step.next_status == JobStatus.COMPLETED


def test_a_delivered_unwraps_deadline_names_the_toll_wait():
    """[review] The RELAYING deadline on a delivered unwrap is a hand-over
    about the replaced toll send, not an undelivered relay."""
    store = new_store()
    engine, _ctx = build_unwrap(store, params=params_with_cap())
    limit = S._PENDING_MAX_POLLS[JobStatus.RELAYING]
    reason = engine._pending_expired(SimpleNamespace(
        status=JobStatus.RELAYING,
        state={"pending_polls": limit, "delivered_by": "our relay"}))
    assert "the unwrap was delivered" in reason and "Sweep recovers" in reason
    plain = engine._pending_expired(SimpleNamespace(
        status=JobStatus.RELAYING, state={"pending_polls": limit}))
    assert "delivered" not in plain


def test_retry_after_delivery_keeps_the_mined_relay():
    """[review] A fresh relay would only revert "!nonce" and burn its gas."""
    store = new_store()
    engine, _ctx = build_unwrap(store, params=params_with_cap())
    job = seed(store, JobStatus.FAILED, columns={"amount_mojos": AMOUNT},
               state={"direction": "out", "failed_from": JobStatus.RELAYING,
                      "relay_raw": "02ee", "relay_tx_hash": "0x" + "77" * 32,
                      "delivered_by": "our relay"})
    engine.job_action(job.id, "retry")
    after = store.get_job(job.id)
    assert after.status == JobStatus.RELAYING
    assert after.state["relay_raw"] == "02ee"
    assert after.state["relay_tx_hash"] == "0x" + "77" * 32


# --------------------------------------------------------------------------- #
# Review round 11.
# --------------------------------------------------------------------------- #

def _job_with_a_landed_replaced_toll(store):
    job = seed(store, JobStatus.RELAYING, columns={"amount_mojos": AMOUNT},
               state={"direction": "out", "receiver_evm": "ab" * 20})
    blob, _sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={
        "ephemeral_blob": blob, "funding_amount": NET.chia_toll_mojos,
        "funding_tx_id": "toll-b", "funding_tx_id_removals": [RID2],
        "funding_tx_id_prior": ["toll-a"],
        "funding_tx_id_prior_removals": {"toll-a": [RID]}})
    return store.get_active_job()


def test_a_recovered_coins_spend_settles_only_at_depth():
    """[review] A one-block-deep sweep or claim can be reorged out; the job
    would then be closed with XCH at the key and no Sweep."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)              # A landed...
    ctx.coinset.records[RID2] = _rec(spent=True)             # B dead
    swept = SimpleNamespace(                                 # ...its coin swept
        coin=SimpleNamespace(parent_coin_info=RID, puzzle_hash="5b" * 32,
                             amount=NET.chia_toll_mojos),
        spent=True, confirmed_block_index=PEAK - 3, spent_block_index=PEAK)
    ctx.coinset.by_ph = [swept]

    settled, note = engine._prior_sends_settled(job)         # 1 confirmation
    assert not settled and "1/32 confirmations" in note

    swept.spent_block_index = 0                              # depth unknown
    settled, note = engine._prior_sends_settled(job)
    assert not settled and "unknown" in note

    swept.spent_block_index = PEAK - 40                      # 41 confirmations
    settled, _note = engine._prior_sends_settled(job)
    assert settled


def test_an_earlier_burns_spent_cat_settles_only_at_depth():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    burn_ph = store.get_active_job().state["burn_cat_ph"]
    store.update_job(job.id, state_patch={
        "burn_tx_id_prior": ["burn-a"],
        "burn_tx_id_prior_removals": {"burn-a": [RID]}})
    job = store.get_active_job()
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)
    cat = SimpleNamespace(
        coin=SimpleNamespace(parent_coin_info=RID, puzzle_hash=burn_ph, amount=AMOUNT),
        spent=True, confirmed_block_index=PEAK - 3, spent_block_index=PEAK)
    ctx.coinset.by_ph = [cat]
    settled, note = engine._burn_priors_settled(job)
    assert not settled and "1/32 confirmations" in note
    cat.spent_block_index = PEAK - 40
    assert engine._burn_priors_settled(job)[0]


def test_retry_keeps_the_coins_an_unnamed_send_spent(monkeypatch):
    """[review] The daemon named no transaction but listed the coins it
    spent: the replacement must exclude them, and Sweep must be able to
    judge the attempt by them."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_tx_id=S._FUNDING_TX_UNKNOWN,
                        funding_amount=EXPECTED, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=False)             # an orphan, by its coins
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.FUNDING_CLAIM,
                     state_patch={"failed_from": JobStatus.FUNDING_CLAIM})
    engine.job_action(job.id, "retry")
    st = store.get_job(job.id).state
    assert st["funding_tx_id"] is None
    assert st["funding_tx_id_exclude"] == [RID]
    k1, k2 = S._FUNDING_TX_UNKNOWN + "#1", S._FUNDING_TX_UNKNOWN + "#2"
    assert st["funding_tx_id_prior"] == [k1]
    assert st["funding_tx_id_prior_removals"] == {k1: [RID]}

    engine.step()                                            # the replacement
    assert ctx.wallet.sent_excludes[-1] == [RID]
    job = store.get_active_job()
    # [review] A second unnamed attempt is retired under its OWN key, so
    # each is settled on its own evidence; only the exclusions are pooled.
    store.update_job(job.id, state_patch={"funding_tx_id": S._FUNDING_TX_UNKNOWN,
                                          "funding_tx_id_removals": [RID2]})
    ctx.coinset.records[RID2] = _rec(spent=False)
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.FUNDING_CLAIM,
                     state_patch={"failed_from": JobStatus.FUNDING_CLAIM})
    engine.job_action(job.id, "retry")
    st = store.get_job(job.id).state
    assert st["funding_tx_id_prior"] == [k1, k2]
    assert st["funding_tx_id_prior_removals"] == {k1: [RID], k2: [RID2]}
    assert st["funding_tx_id_exclude"] == sorted([RID, RID2])


def test_an_unnamed_attempt_with_known_coins_is_judged_for_settlement():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": [S._FUNDING_TX_UNKNOWN],
        "funding_tx_id_prior_removals": {S._FUNDING_TX_UNKNOWN: [RID]}})
    job = store.get_active_job()
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)              # its coin went elsewhere
    ctx.coinset.records[RID2] = _rec(spent=True)
    assert engine._prior_sends_settled(job)[0]
    store.update_job(job.id, state_patch={"funding_tx_id_prior_removals": None})
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "not known" in note


# --------------------------------------------------------------------------- #
# Review round 12.
# --------------------------------------------------------------------------- #

def test_unnamed_attempts_settle_independently():
    """[review] One attempt's dead input must not settle another that could
    still land; a legacy entry that merged several is dead only when every
    coin is spent at depth."""
    k1, k2 = S._FUNDING_TX_UNKNOWN + "#1", S._FUNDING_TX_UNKNOWN + "#2"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": [k1, k2],
        "funding_tx_id_prior_removals": {k1: [RID], k2: [RID2]}})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)              # #1 dead
    ctx.coinset.records[RID2] = _rec(spent=False)            # #2 could land
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "could still land" in note
    ctx.coinset.records[RID2] = _rec(spent=True)
    assert engine._prior_sends_settled(store.get_active_job())[0]

    # A merged legacy entry: conservative.
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": [S._FUNDING_TX_UNKNOWN],
        "funding_tx_id_prior_removals": {S._FUNDING_TX_UNKNOWN: [RID, RID2]}})
    ctx.coinset.records[RID2] = _rec(spent=False)
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "share one record" in note
    ctx.coinset.records[RID2] = _rec(spent=True)
    assert engine._prior_sends_settled(store.get_active_job())[0]


def test_the_current_unnamed_attempt_is_judged_by_its_own_coins():
    k1 = S._FUNDING_TX_UNKNOWN + "#1"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    store.update_job(job.id, state_patch={
        "funding_tx_id": S._FUNDING_TX_UNKNOWN, "funding_tx_id_removals": [RID2],
        "funding_tx_id_prior": [k1], "funding_tx_id_prior_removals": {k1: [RID]}})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.records[RID2] = _rec(spent=False)            # current could land
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "could still land" in note
    ctx.coinset.records[RID2] = _rec(spent=True)
    assert engine._prior_sends_settled(store.get_active_job())[0]


def test_an_unnamed_cat_spend_whose_coins_went_elsewhere_is_sent_again():
    """[review] A history miss is not the end: the recorded coins, spent
    elsewhere at depth with no burn CAT of its own, prove it dead."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={"burn_tx_id": S._FUNDING_TX_UNKNOWN,
                                          "burn_tx_id_removals": [RID]})
    ctx.coinset.records[RID] = _rec(spent=False)
    engine.step()
    assert ctx.wallet.cat_spends == []
    assert "waiting" in _last_message(store, job.id)
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK
    engine.step()
    assert len(ctx.wallet.cat_spends) == 1
    assert ctx.wallet.cat_spend_excludes == [[RID]]
    st = store.get_active_job().state
    assert st["burn_tx_id"] == "burn-tx-1"
    assert st["burn_tx_id_prior"] == [S._FUNDING_TX_UNKNOWN + "#1"]
    assert st["burn_tx_id_prior_removals"] == {S._FUNDING_TX_UNKNOWN + "#1": [RID]}


def test_retry_replaces_an_unnamed_cat_spend_only_when_dead():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    store.update_job(job.id, state_patch={"burn_tx_id": S._FUNDING_TX_UNKNOWN,
                                          "burn_tx_id_removals": [RID]})
    ctx.coinset.records[RID] = _rec(spent=False)
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.BURN_SENT,
                     state_patch={"failed_from": JobStatus.BURN_SENT})
    engine.job_action(job.id, "retry")
    assert store.get_job(job.id).state["burn_tx_id"] == S._FUNDING_TX_UNKNOWN
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.BURN_SENT,
                     state_patch={"failed_from": JobStatus.BURN_SENT})
    engine.job_action(job.id, "retry")
    st = store.get_job(job.id).state
    assert st["burn_tx_id"] is None
    assert st["burn_tx_id_exclude"] == [RID]
    assert st["burn_tx_id_prior"] == [S._FUNDING_TX_UNKNOWN + "#1"]


# --------------------------------------------------------------------------- #
# Review round 13.
# --------------------------------------------------------------------------- #

def test_an_unnamed_sends_evidence_never_queries_the_sentinel(monkeypatch):
    """[review] The hash RPCs reject the sentinel; that error must not keep
    an orphaned unnamed funding send UNKNOWN on every Retry."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_tx_id=S._FUNDING_TX_UNKNOWN,
                        funding_amount=EXPECTED, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=False)

    def invalid_hex(tx_id):
        raise RuntimeError(f"invalid hex: {tx_id}")

    ctx.wallet.get_transaction_or_none = invalid_hex
    ctx.coinset.get_mempool_item_by_tx_id = invalid_hex
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.FUNDING_CLAIM,
                     state_patch={"failed_from": JobStatus.FUNDING_CLAIM})
    engine.job_action(job.id, "retry")
    st = store.get_job(job.id).state
    assert st["funding_tx_id"] is None
    assert st["funding_tx_id_prior"] == [S._FUNDING_TX_UNKNOWN + "#1"]


def test_a_legacy_merged_entry_settles_as_landed_only_with_every_coin_spent():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": [S._FUNDING_TX_UNKNOWN],
        "funding_tx_id_prior_removals": {S._FUNDING_TX_UNKNOWN: [RID, RID2]}})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)              # A landed, swept at depth
    ctx.coinset.by_ph = [SimpleNamespace(
        coin=SimpleNamespace(parent_coin_info=RID, puzzle_hash="5b" * 32,
                             amount=NET.chia_toll_mojos),
        spent=True, confirmed_block_index=PEAK - 50, spent_block_index=PEAK - 40)]
    ctx.coinset.records[RID2] = _rec(spent=False)            # B could still land
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "share one record" in note
    ctx.coinset.records[RID2] = _rec(spent=True)
    assert engine._prior_sends_settled(store.get_active_job())[0]


def test_a_dead_replacement_is_not_read_as_landed_by_a_swept_priors_coin(monkeypatch):
    """[review] A replaced send A landed and was swept; the replacement B
    then died.  A's spent output must not make B read LANDED."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id="fund-b", funding_amount=EXPECTED,
                  funding_tx_id_removals=[RID2],
                  funding_tx_id_prior=["fund-a"],
                  funding_tx_id_prior_removals={"fund-a": [RID]})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.records[RID2] = _rec(spent=True)             # B: dead
    ctx.coinset.by_ph = [SimpleNamespace(                    # A's coin, swept
        coin=SimpleNamespace(parent_coin_info=RID, puzzle_hash=SECURITY_PH.hex(),
                             amount=EXPECTED),
        spent=True, confirmed_block_index=PEAK - 50, spent_block_index=PEAK - 40)]
    engine.step()
    assert [s[0] for s in ctx.wallet.sent] == [EXPECTED]
    st = store.get_active_job().state
    assert st["funding_tx_id"] == "funding-tx-1"
    assert st["funding_tx_id_prior"] == ["fund-a", "fund-b"]


# --------------------------------------------------------------------------- #
# Review round 14.
# --------------------------------------------------------------------------- #

def test_a_current_multi_input_unnamed_send_is_not_a_legacy_group():
    """[review] One unnamed send that spent two coins is dead once either is
    spent elsewhere at depth with no output; only a legacy entry that merged
    several attempts needs every coin spent."""
    k1 = S._FUNDING_TX_UNKNOWN + "#1"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    store.update_job(job.id, state_patch={
        "funding_tx_id": S._FUNDING_TX_UNKNOWN,
        "funding_tx_id_removals": [RID, RID2],
        "funding_tx_id_prior": [k1],
        "funding_tx_id_prior_removals": {k1: ["f" * 64]}})
    ctx.coinset.peak = PEAK
    ctx.coinset.records["f" * 64] = _rec(spent=True)
    ctx.coinset.records[RID] = _rec(spent=True)              # one input gone elsewhere
    ctx.coinset.records[RID2] = _rec(spent=False)            # the other still unspent
    assert engine._prior_sends_settled(store.get_active_job())[0]


def test_an_uncertain_sweep_push_keeps_its_fee_pin_through_a_later_rejection(
        monkeypatch):
    """[review] A timed-out push may have reached a node; a later fee
    rejection must not reprice over the bundle it may still hold."""
    from gui.services.warp.coinset import CoinsetError

    fees = []
    outcome = {"error": "push_tx request failed: timed out"}

    def push(coinset, net, *, security_coin, destination_puzzle_hash,
             ephemeral_sk, sweep_fee=0):
        fees.append(sweep_fee)
        if outcome["error"]:
            raise CoinsetError(outcome["error"])
        return ("accepted", "ok")

    monkeypatch.setattr(claim_mod, "build_and_push_sweep", push)
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: False)
    store = new_store()
    engine, ctx = build(store, params=default_params(offer_fee_mojos=10_000_000))
    job = _seed_funding(store, funding_amount=EXPECTED)
    resolved, status = engine._sweep_security(job, FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert not resolved and "timed out" in status
    st = store.get_job(job.id).state
    assert st["sweep_fee_by_coin"] == {COIN: 10_000_000}
    assert st["sweep_pushed_by_coin"] == {COIN: "uncertain"}

    outcome["error"] = "push_tx failed: [INVALID_FEE_LOW_FEE] Failed to include transaction"
    ctx.coinset.fee_estimates = [50_000_000]
    engine._sweep_security(store.get_job(job.id), FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert fees == [10_000_000, 10_000_000], "the same bundle, not a repriced twin"
    assert store.get_job(job.id).state["sweep_fee_by_coin"] == {COIN: 10_000_000}

    outcome["error"] = None
    engine._sweep_security(store.get_job(job.id), FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert store.get_job(job.id).state["sweep_pushed_by_coin"] == {COIN: True}


# --------------------------------------------------------------------------- #
# Review round 15.
# --------------------------------------------------------------------------- #

def test_a_failed_destination_lookup_records_no_sweep_submission(monkeypatch):
    """[review] No bundle went out, so a later fee rejection may still
    release the pin and reprice."""
    from gui.services.warp.coinset import CoinsetError

    fees = []

    def push(coinset, net, *, security_coin, destination_puzzle_hash,
             ephemeral_sk, sweep_fee=0):
        fees.append(sweep_fee)
        raise CoinsetError("push_tx failed: [INVALID_FEE_LOW_FEE] Failed to include")

    monkeypatch.setattr(claim_mod, "build_and_push_sweep", push)
    store = new_store()
    engine, ctx = build(store, params=default_params(offer_fee_mojos=10_000_000))
    job = _seed_funding(store, funding_amount=EXPECTED)
    real = engine._sweep_destination_ph
    monkeypatch.setattr(engine, "_sweep_destination_ph",
                        lambda: (_ for _ in ()).throw(RuntimeError("wallet down")))
    resolved, status = engine._sweep_security(job, FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert not resolved and "wallet down" in status and fees == []
    st = store.get_job(job.id).state
    assert not (st.get("sweep_pushed_by_coin") or {})
    monkeypatch.setattr(engine, "_sweep_destination_ph", real)
    resolved, status = engine._sweep_security(store.get_job(job.id),
                                              FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert "repriced" in status
    assert store.get_job(job.id).state["sweep_fee_by_coin"] == {}


def test_an_unnamed_cat_spend_found_in_history_keeps_its_output_pin():
    """[review] The history scan names the burn CAT too; pinned, a later
    scan cannot adopt an unrelated CAT of the same receiver and amount."""
    from gui.services.warp import clvm_utils as cu

    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    burn_ph = store.get_active_job().state["burn_cat_ph"]
    store.update_job(job.id, state_patch={"burn_tx_id": S._FUNDING_TX_UNKNOWN})
    ctx.wallet.cat_txs = [{"name": "hist-burn", "additions": [
        {"puzzle_hash": burn_ph, "amount": AMOUNT, "parent_coin_info": RID}]}]
    engine.step()
    st = store.get_active_job().state
    assert st["burn_tx_id"] == "hist-burn"
    assert st["burn_cat_coin_id"] == cu.coin_name(
        bytes.fromhex(RID), bytes.fromhex(burn_ph), AMOUNT).hex()
    assert ctx.wallet.cat_spends == []
