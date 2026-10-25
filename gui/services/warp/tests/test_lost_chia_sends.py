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


@pytest.fixture(autouse=True)
def _grace_out_of_reach(monkeypatch):
    """The default test clock jumps 100,000 s per call (test_warp_service
    big_clock), which would run out the six-hour absence grace between two
    ticks; tests about the grace set the real value themselves."""
    monkeypatch.setattr(S, "_UNOBSERVABLE_ATTEMPT_GRACE_S", 10 ** 9)


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
    # [review] Zero or negative (the engine refuses both at startup) must not
    # disable the automatic fee: a zero floor would cap any estimate at zero.
    assert S._offer_fee_floor_mojos({"strategy": {"offer_fee_mojos": 0}}) == 10_000_000
    assert S._offer_fee_floor_mojos({"strategy": {"offer_fee_mojos": -5}}) == 10_000_000
    assert S.warp_params_from_config(
        {"warp": {"enabled": False}, "strategy": {"offer_fee_mojos": 0}}
    ).offer_fee_mojos == 10_000_000
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
    # [review] A later DEFINITIVE rejection of the identical bundle means the
    # node no longer holds it (one it held would answer "pending"), and the
    # coin is still unspent: the pin is released and the next Sweep reprices.
    outcome["raise"] = True
    ctx.coinset.fee_estimates = [50_000_000]
    engine._sweep_security(store.get_job(job.id), FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert fees[-1] == 30_000_000
    assert store.get_job(job.id).state["sweep_fee_by_coin"] == {}
    outcome["raise"] = False
    engine._sweep_security(store.get_job(job.id), FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert fees[-1] == 50_000_000


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


def test_retrys_burn_replacement_keeps_the_dead_sends_coins_on_the_books():
    """[review] A dead send can never confirm, so its coins are NOT excluded
    from the replacement (one conflicting input made the verdict; the rest
    stay selectable) -- but they stay on the books for the accounting."""
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
    st = store.get_job(job.id).state
    assert st["burn_tx_id_exclude"] is None
    assert st["burn_tx_id_prior_dead"] == ["burn-dead"]
    engine.step()
    assert ctx.wallet.cat_spend_excludes == [[]]
    st = store.get_active_job().state
    assert st["burn_tx_id"] == "burn-tx-1"
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
    """[review] A replaced by B, B replaced by C: every attempt stays on
    the books, and C excludes the coins of each attempt that could still
    land (none here: all proven dead) -- never a dead one's."""
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
    assert st["burn_tx_id_prior"] == ["burn-a", "burn-b", "burn-c"]
    assert st["burn_tx_id_prior_removals"] == {
        "burn-a": [RID], "burn-b": [RID2], "burn-c": ["f" * 64]}
    # burn-a, a legacy prior, was re-judged on the tick that sent C and
    # proven dead there ([review]); nothing is left to exclude.
    assert st["burn_tx_id_prior_dead"] == ["burn-b", "burn-a", "burn-c"]
    assert st["burn_tx_id_exclude"] is None
    engine.step()
    assert ctx.wallet.cat_spend_excludes == [[]]


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
    assert not settled and "presumed gone" in note


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
    assert ctx.wallet.cat_spend_excludes == [[]], "a dead send's coins are free"
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
    assert st["burn_tx_id_exclude"] is None
    assert st["burn_tx_id_prior_dead"] == [S._FUNDING_TX_UNKNOWN + "#1"]
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


def test_an_uncertain_sweep_push_is_retried_identically_until_the_node_decides(
        monkeypatch):
    """[review] A timed-out push may have reached the node: the next Sweep
    re-pushes the IDENTICAL bundle (same fee).  A definitive fee rejection
    of that bundle proves the node holds no copy, so the pin is released
    and the Sweep after reprices -- the coin is never left unsweepable."""
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
    assert fees == [10_000_000, 10_000_000], "the same bundle first, not a repriced twin"
    st = store.get_job(job.id).state
    assert st["sweep_fee_by_coin"] == {} and not st.get("sweep_pushed_by_coin")

    outcome["error"] = None
    engine._sweep_security(store.get_job(job.id), FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert fees[-1] == 50_000_000, "repriced once the node proved it held nothing"
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


# --------------------------------------------------------------------------- #
# Review round 16.
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("status", [JobStatus.AWAITING_EXTERNAL_RELAY, JobStatus.RELAYING])
def test_a_delivered_unwrap_resumes_settlement_without_relay_prerequisites(status):
    """[review] A threshold change must not send an already-delivered
    message back to signature collection while replaced toll sends settle."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = seed(store, status, columns={"amount_mojos": AMOUNT},
               state={"direction": "out", "receiver_evm": "ab" * 20,
                      "post_tip_base_units": 4_985_000, "delivered_by": "our relay",
                      "relay_threshold": 6, "relay_raw": "02ee"})
    blob, _sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={
        "ephemeral_blob": blob, "funding_amount": NET.chia_toll_mojos,
        "funding_tx_id": "toll-b", "funding_tx_id_removals": [RID2],
        "funding_tx_id_prior": ["toll-a"],
        "funding_tx_id_prior_removals": {"toll-a": [RID]}})
    ctx.evm.threshold = 7                                    # changed since collection
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID2] = _rec(spent=True)
    ctx.coinset.records[RID] = _rec(spent=False)             # toll-a could still land
    out = engine.step()
    assert out["status"] == status
    assert "could still land" in _last_message(store, job.id)
    ctx.coinset.records[RID] = _rec(spent=True)
    out = engine.step()
    assert out["status"] == JobStatus.COMPLETED


# --------------------------------------------------------------------------- #
# Review round 17.
# --------------------------------------------------------------------------- #

def test_a_sweep_is_not_broadcast_unless_its_records_are_written(monkeypatch):
    """[review] An unrecorded fee pin or submission marker would let a later
    Sweep reprice over a bundle a node may hold: no push without them."""
    pushes = []
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: pushes.append(1) or ("accepted", "ok"))
    store = new_store()
    engine, ctx = build(store, params=default_params(offer_fee_mojos=10_000_000))
    job = _seed_funding(store, funding_amount=EXPECTED)
    real = store.update_job

    def fail_on(key):
        def update(job_id, **kw):
            if key in (kw.get("state_patch") or {}):
                raise RuntimeError("disk full")
            return real(job_id, **kw)
        return update

    monkeypatch.setattr(store, "update_job", fail_on("sweep_fee_by_coin"))
    resolved, status = engine._sweep_security(job, FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert not resolved and "not swept" in status and pushes == []

    monkeypatch.setattr(store, "update_job", real)
    monkeypatch.setattr(store, "update_job", fail_on("sweep_pushed_by_coin"))
    resolved, status = engine._sweep_security(store.get_job(job.id),
                                              FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert not resolved and "not swept" in status and pushes == []

    monkeypatch.setattr(store, "update_job", real)
    engine._sweep_security(store.get_job(job.id), FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert pushes == [1]


# --------------------------------------------------------------------------- #
# Review round 19.
# --------------------------------------------------------------------------- #

def test_only_attempts_that_could_still_land_are_excluded(monkeypatch):
    """[review] An orphan's coins are excluded (it may surface); a dead
    multi-input send's surviving coins are not (it can never confirm), so
    replacements are never starved of funds."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_tx_id="fund-a", funding_amount=EXPECTED,
                        funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=False)             # A: an orphan
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.FUNDING_CLAIM,
                     state_patch={"failed_from": JobStatus.FUNDING_CLAIM})
    engine.job_action(job.id, "retry")
    st = store.get_job(job.id).state
    assert st["funding_tx_id_exclude"] == [RID] and not st.get("funding_tx_id_prior_dead")
    engine.step()                                            # B sent, excluding A's coin
    assert ctx.wallet.sent_excludes[-1] == [RID]
    job = store.get_active_job()
    other = "a" * 64
    store.update_job(job.id, state_patch={"funding_tx_id": "fund-b",
                                          "funding_tx_id_removals": [RID2, other]})
    ctx.coinset.records[RID2] = _rec(spent=True)             # B: one input gone...
    ctx.coinset.records[other] = _rec(spent=False)           # ...the other survives
    ctx.coinset.peak = PEAK
    engine.step()                                            # B dead -> C sent
    assert len(ctx.wallet.sent) == 2
    assert ctx.wallet.sent_excludes[-1] == [RID], "only the orphan's coin"
    st = store.get_active_job().state
    assert st["funding_tx_id_prior"] == ["fund-a", "fund-b"]
    assert st["funding_tx_id_prior_dead"] == ["fund-b"]
    assert st["funding_tx_id_prior_removals"] == {"fund-a": [RID],
                                                  "fund-b": [RID2, other]}


# --------------------------------------------------------------------------- #
# Review round 21.
# --------------------------------------------------------------------------- #

def test_an_unpinned_scan_adopts_only_a_coin_with_provenance():
    """[review] After a replacement whose response named no coin, an
    unrelated CAT of the same receiver and amount is not adopted; a coin
    the wallet history names as our cat_spend's output is."""
    from gui.services.warp import clvm_utils as cu

    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    burn_ph = store.get_active_job().state["burn_cat_ph"]
    store.update_job(job.id, state_patch={
        "burn_tx_id": "burn-b", "burn_tx_id_removals": None,   # inputs unknown
        "burn_cat_coin_id": None,
        "burn_tx_id_prior": ["burn-a"],
        "burn_tx_id_prior_removals": {"burn-a": [RID]},
        "burn_tx_id_prior_dead": ["burn-a"]})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)
    stranger = SimpleNamespace(                              # someone else's CAT
        coin=SimpleNamespace(parent_coin_info="cc" * 32, puzzle_hash=burn_ph,
                             amount=AMOUNT),
        spent=False, confirmed_block_index=PEAK - 3, spent_block_index=0)
    ctx.coinset.by_ph = [stranger]
    out = engine.step()
    assert out["status"] == JobStatus.BURN_SENT
    assert ctx.wallet.cat_spends == []

    ours = SimpleNamespace(
        coin=SimpleNamespace(parent_coin_info="dd" * 32, puzzle_hash=burn_ph,
                             amount=AMOUNT),
        spent=False, confirmed_block_index=PEAK - 2, spent_block_index=0)
    ctx.coinset.by_ph = [stranger, ours]
    ctx.wallet.cat_txs = [{"name": "burn-b", "additions": [
        {"puzzle_hash": burn_ph, "amount": AMOUNT, "parent_coin_info": "dd" * 32}]}]
    out = engine.step()
    assert out["status"] == JobStatus.BURNING
    st = store.get_active_job().state
    assert st["burn_cat_parent"] == "dd" * 32
    assert st["burn_cat_coin_id"] == cu.coin_name(
        bytes.fromhex("dd" * 32), bytes.fromhex(burn_ph), AMOUNT).hex()


def test_the_third_party_checkpoint_survives_a_failing_settlement_check(monkeypatch):
    """[review] The resolved sweep is on disk before the settlement check,
    which can raise; the next tick resumes from it."""
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "claim_landed", lambda coinset, cid: False)
    monkeypatch.setattr(claim_mod, "sync_portal",
                        lambda coinset, net, *, hint, max_hops=64:
                        SimpleNamespace(coin_id=b"\x50" * 32))
    monkeypatch.setattr(claim_mod, "build_and_push_claim",
                        lambda coinset, net, **kw: SimpleNamespace(
                            claim=SimpleNamespace(final_cat_coin_id=b"\xf4" * 32),
                            accepted=False, status="conflict: coin already spent"))
    monkeypatch.setattr(claim_mod, "message_claimed_on_chain", lambda *a, **k: True)
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: ("accepted", "ok"))
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    store = new_store()
    engine, ctx = build(store)
    job = seed(store, JobStatus.CLAIMING,
               columns={"bridge_nonce": "00" * 31 + "07", "post_tip_mojos": 4985,
                        "receiver_ph": "ab" * 32},
               state={"message_destination": "00" * 32,
                      "message_contents": ["aa", "bb", "cc"],
                      "portal_coin_id": (b"\x50" * 32).hex(),
                      "sigs": {str(i): bytes([0xC0 + i]).hex() for i in range(6)},
                      "security_coin_id": (b"\x5a" * 32).hex()})
    blob, _sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={
        "ephemeral_blob": blob, "funding_amount": EXPECTED,
        "funding_tx_id": "replacement-tx", "funding_tx_id_removals": [RID2],
        "funding_tx_id_prior": [DEAD_TX],
        "funding_tx_id_prior_removals": {DEAD_TX: [RID]}})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=False)             # the orphan could land
    ctx.coinset.records[RID2] = _rec(spent=True)
    real = engine._prior_sends_settled
    boom = {"on": True}

    def flaky(j, *a, **k):
        if boom["on"]:
            raise RuntimeError("wallet rpc down")
        return real(j, *a, **k)

    monkeypatch.setattr(engine, "_prior_sends_settled", flaky)
    out = engine.step()                                      # the check raised...
    assert out["status"] == JobStatus.CLAIMING and out["retry_count"] == 1
    st = store.get_job(job.id).state
    assert st.get("third_party_claim") and st.get("swept"), "...but the sweep is recorded"
    boom["on"] = False
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    store.update_job(job.id, columns={"next_retry_at": None})
    out = engine.step()                                      # resumes from the checkpoint
    assert out["status"] == JobStatus.CLAIMING
    assert "could still land" in _last_message(store, job.id)


# --------------------------------------------------------------------------- #
# Review round 22.
# --------------------------------------------------------------------------- #

def test_only_the_daemons_own_not_found_reads_as_absence():
    """[review] A transport 404 is not the daemon saying "gone"."""
    from gui.services.warp.wallet import WalletClient, WalletError

    client = WalletClient.__new__(WalletClient)
    answers = {}
    client._post = lambda endpoint, body: (_ for _ in ()).throw(answers["exc"])
    answers["exc"] = WalletError("get_transaction failed: Transaction 0xab not found")
    assert client.get_transaction_or_none("ab" * 32) is None
    answers["exc"] = WalletError("get_transaction request failed: HTTP 404 Not Found")
    with pytest.raises(WalletError):
        client.get_transaction_or_none("ab" * 32)
    answers["exc"] = WalletError("get_transaction request failed: timed out")
    with pytest.raises(WalletError):
        client.get_transaction_or_none("ab" * 32)


def test_the_history_lookup_for_provenance_selects_the_configured_key_first():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    burn_ph = store.get_active_job().state["burn_cat_ph"]
    store.update_job(job.id, state_patch={
        "burn_tx_id": "burn-b", "burn_tx_id_removals": None,
        "burn_cat_coin_id": None, "burn_tx_id_prior": ["burn-a"],
        "burn_tx_id_prior_removals": {"burn-a": [RID]},
        "burn_tx_id_prior_dead": ["burn-a"]})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.by_ph = [SimpleNamespace(
        coin=SimpleNamespace(parent_coin_info="dd" * 32, puzzle_hash=burn_ph,
                             amount=AMOUNT),
        spent=False, confirmed_block_index=PEAK - 2, spent_block_index=0)]
    ctx.wallet.cat_txs = [{"name": "burn-b", "additions": [
        {"puzzle_hash": burn_ph, "amount": AMOUNT, "parent_coin_info": "dd" * 32}]}]
    order = []
    real_login, real_txs = ctx.wallet.log_in, ctx.wallet.get_transactions
    ctx.wallet.log_in = lambda: order.append("log_in") or real_login()
    ctx.wallet.get_transactions = lambda *a, **k: order.append("history") or real_txs(*a, **k)
    out = engine.step()
    assert out["status"] == JobStatus.BURNING
    assert order.index("log_in") < order.index("history")


def test_delivery_is_recorded_before_the_fallible_settlement_lookups(monkeypatch):
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    monkeypatch.setattr(engine, "_prior_sends_settled",
                        lambda j, *a, **k: (_ for _ in ()).throw(RuntimeError("wallet down")))
    with pytest.raises(RuntimeError):
        engine._unwrap_delivered(job, by="our relay", tx_hash="0x" + "77" * 32)
    after = store.get_job(job.id)
    assert after.state["delivered_by"] == "our relay"
    assert after.bridge_tx_hash == "0x" + "77" * 32


# --------------------------------------------------------------------------- #
# Review round 23.
# --------------------------------------------------------------------------- #

def test_a_retained_orphans_coins_are_released_once_it_is_proven_dead(monkeypatch):
    """[review] Retry retired an orphan with its coins excluded; a later
    conflicting spend proves it dead before the replacement goes out, so
    the replacement may use its surviving coins."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_tx_id="fund-a", funding_amount=EXPECTED,
                        funding_tx_id_removals=[RID, "a" * 64])
    ctx.coinset.records[RID] = _rec(spent=False)
    ctx.coinset.records["a" * 64] = _rec(spent=False)
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.FUNDING_CLAIM,
                     state_patch={"failed_from": JobStatus.FUNDING_CLAIM})
    engine.job_action(job.id, "retry")
    assert store.get_job(job.id).state["funding_tx_id_exclude"] == sorted([RID, "a" * 64])
    ctx.coinset.records[RID] = _rec(spent=True)              # one input went elsewhere
    ctx.coinset.peak = PEAK
    engine.step()                                            # the replacement
    assert ctx.wallet.sent_excludes[-1] == [], "the dead orphan's coins are free"
    st = store.get_active_job().state
    assert st["funding_tx_id_prior_dead"] == ["fund-a"]
    assert st["funding_tx_id_prior_removals"] == {"fund-a": [RID, "a" * 64]}


def test_an_unnamed_current_attempt_is_identified_from_the_history_for_settlement():
    """[review] Its coin appeared before the tick that would have adopted the
    id: the wallet history names it, its coins decide, and the marker is
    persisted under that name."""
    from gui.services.warp import chia_send as cs

    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    ph = claim_mod.security_coin_puzzle_hash(engine._load_ephemeral_sk(job)).hex()
    store.update_job(job.id, state_patch={
        "funding_tx_id": S._FUNDING_TX_UNKNOWN, "funding_tx_id_removals": None})
    removal = {"parent_coin_info": "d4" * 32, "puzzle_hash": "11" * 32,
               "amount": 499_005_000_000}
    rid = cs.coin_id("d4" * 32, "11" * 32, 499_005_000_000)
    ctx.wallet.txs = [{"name": "toll-x", "confirmed": True, "removals": [removal],
                       "additions": [{"puzzle_hash": ph, "amount": NET.chia_toll_mojos}]}]
    ctx.wallet.tx_records["toll-x"] = {"confirmed": True, "removals": [removal]}
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)              # toll-a: dead
    ctx.coinset.records[rid] = _rec(spent=True)              # toll-x landed...
    ctx.coinset.by_ph = [SimpleNamespace(                    # ...its coin spent at depth
        coin=SimpleNamespace(parent_coin_info=rid, puzzle_hash=ph,
                             amount=NET.chia_toll_mojos),
        spent=True, confirmed_block_index=PEAK - 50, spent_block_index=PEAK - 40)]
    assert engine._prior_sends_settled(store.get_active_job())[0]
    st = store.get_job(job.id).state
    assert st["funding_tx_id"] == "toll-x"
    assert st["funding_tx_id_removals"] == [rid]
    # Unidentifiable: still not ruled out.
    store.update_job(job.id, state_patch={
        "funding_tx_id": S._FUNDING_TX_UNKNOWN, "funding_tx_id_removals": None})
    ctx.wallet.txs = []
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "not known" in note


# --------------------------------------------------------------------------- #
# Review round 24.
# --------------------------------------------------------------------------- #

def test_one_history_transaction_identifies_at_most_one_attempt():
    """[review] Two unnamed attempts with no coins and one matching history
    transaction: one is identified, the other stays unresolved; a second
    transaction settles both."""
    from gui.services.warp import chia_send as cs

    k1, k2 = S._FUNDING_TX_UNKNOWN + "#1", S._FUNDING_TX_UNKNOWN + "#2"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    ph = claim_mod.security_coin_puzzle_hash(engine._load_ephemeral_sk(job)).hex()
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": [k1, k2], "funding_tx_id_prior_removals": None})

    def tx(name, parent):
        removal = {"parent_coin_info": parent, "puzzle_hash": "11" * 32,
                   "amount": 499_005_000_000}
        rid = cs.coin_id(parent, "11" * 32, 499_005_000_000)
        ctx.wallet.tx_records[name] = {"confirmed": True, "removals": [removal]}
        ctx.coinset.records[rid] = _rec(spent=True)
        return ({"name": name, "confirmed": True, "removals": [removal],
                 "additions": [{"puzzle_hash": ph, "amount": NET.chia_toll_mojos}]},
                SimpleNamespace(coin=SimpleNamespace(parent_coin_info=rid, puzzle_hash=ph,
                                                     amount=NET.chia_toll_mojos),
                                spent=True, confirmed_block_index=PEAK - 50,
                                spent_block_index=PEAK - 40))

    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID2] = _rec(spent=True)             # the current attempt: dead
    tx1, out1 = tx("toll-x", "d4" * 32)
    ctx.wallet.txs = [tx1]
    ctx.coinset.by_ph = [out1]
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "presumed gone" in note, "toll-x cannot settle both"

    tx2, out2 = tx("toll-y", "d5" * 32)
    ctx.wallet.txs = [tx1, tx2]
    ctx.coinset.by_ph = [out1, out2]
    assert engine._prior_sends_settled(store.get_active_job())[0]


def test_a_transaction_already_on_the_books_is_not_handed_to_an_unnamed_attempt():
    """[review] A history transaction whose coins are recorded under some
    attempt is that attempt, not the unnamed one's."""
    from gui.services.warp import chia_send as cs

    k1 = S._FUNDING_TX_UNKNOWN + "#1"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    ph = claim_mod.security_coin_puzzle_hash(engine._load_ephemeral_sk(job)).hex()
    removal = {"parent_coin_info": "d4" * 32, "puzzle_hash": "11" * 32,
               "amount": 499_005_000_000}
    rid = cs.coin_id("d4" * 32, "11" * 32, 499_005_000_000)
    store.update_job(job.id, state_patch={
        "funding_tx_id": "toll-x", "funding_tx_id_removals": [rid],  # named, on the books
        "funding_tx_id_prior": [k1], "funding_tx_id_prior_removals": None})
    ctx.wallet.tx_records["toll-x"] = {"confirmed": True, "removals": [removal]}
    ctx.wallet.txs = [{"name": "toll-x", "confirmed": True, "removals": [removal],
                       "additions": [{"puzzle_hash": ph, "amount": NET.chia_toll_mojos}]}]
    ctx.coinset.peak = PEAK
    ctx.coinset.records[rid] = _rec(spent=True)
    ctx.coinset.by_ph = [SimpleNamespace(
        coin=SimpleNamespace(parent_coin_info=rid, puzzle_hash=ph, amount=NET.chia_toll_mojos),
        spent=True, confirmed_block_index=PEAK - 50, spent_block_index=PEAK - 40)]
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "presumed gone" in note


# --------------------------------------------------------------------------- #
# Review round 25.
# --------------------------------------------------------------------------- #

def test_a_replacement_may_reuse_a_dead_predecessors_surviving_coin():
    """[review] The dead predecessor's own record is never handed to the
    unnamed replacement, but a record that merely reuses the predecessor's
    surviving coin is the replacement's and settles it."""
    from gui.services.warp import chia_send as cs

    k1 = S._FUNDING_TX_UNKNOWN + "#1"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    ph = claim_mod.security_coin_puzzle_hash(engine._load_ephemeral_sk(job)).hex()

    def coin(parent):
        return ({"parent_coin_info": parent, "puzzle_hash": "11" * 32,
                 "amount": 499_005_000_000},
                cs.coin_id(parent, "11" * 32, 499_005_000_000))

    x, xid = coin("d4" * 32)                                 # spent elsewhere (dead)
    y, yid = coin("d5" * 32)                                 # the survivor
    z, zid = coin("d6" * 32)
    store.update_job(job.id, state_patch={
        "funding_tx_id": S._FUNDING_TX_UNKNOWN, "funding_tx_id_removals": None,
        "funding_tx_id_prior": [k1], "funding_tx_id_prior_removals": {k1: [xid, yid]},
        "funding_tx_id_prior_dead": [k1]})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[xid] = _rec(spent=True)
    ctx.coinset.records[RID2] = _rec(spent=True)
    # The dead predecessor's own record is still in the history (the wallet
    # keeps retrying it), FIRST; the replacement reused its survivor.
    ctx.wallet.txs = [
        {"name": "dead-rec", "confirmed": False, "removals": [x, y],
         "additions": [{"puzzle_hash": ph, "amount": NET.chia_toll_mojos}]},
        {"name": "toll-r", "confirmed": True, "removals": [y, z],
         "additions": [{"puzzle_hash": ph, "amount": NET.chia_toll_mojos}]}]
    ctx.wallet.tx_records["dead-rec"] = {"confirmed": False, "removals": [x, y]}
    ctx.wallet.tx_records["toll-r"] = {"confirmed": True, "removals": [y, z]}
    ctx.coinset.records[yid] = _rec(spent=True)
    ctx.coinset.records[zid] = _rec(spent=True)
    ctx.coinset.by_ph = [SimpleNamespace(                    # the replacement landed, swept
        coin=SimpleNamespace(parent_coin_info=yid, puzzle_hash=ph,
                             amount=NET.chia_toll_mojos),
        spent=True, confirmed_block_index=PEAK - 50, spent_block_index=PEAK - 40)]
    assert engine._prior_sends_settled(store.get_active_job())[0]
    st = store.get_job(job.id).state
    assert st["funding_tx_id"] == "toll-r"
    assert sorted(st["funding_tx_id_removals"]) == sorted([yid, zid])

    # Only the dead predecessor's record in the history: not the replacement's.
    store.update_job(job.id, state_patch={
        "funding_tx_id": S._FUNDING_TX_UNKNOWN, "funding_tx_id_removals": None})
    ctx.wallet.txs = ctx.wallet.txs[:1]
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "not known" in note


# --------------------------------------------------------------------------- #
# Review round 26.
# --------------------------------------------------------------------------- #

def test_a_retired_unnamed_attempts_record_is_never_adopted_as_the_replacements():
    """[review] Retired unnamed A (coins X) and unnamed replacement B (coins
    Y): the history hit for A's real record must not become B's id, which
    would discard B's coins and let a DEAD verdict on A authorise a second
    cat_spend while B can still confirm."""
    from gui.services.warp import chia_send as cs

    k1 = S._FUNDING_TX_UNKNOWN + "#1"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _to_burn_sent(store, engine)
    burn_ph = store.get_active_job().state["burn_cat_ph"]

    def coin(parent):
        return ({"parent_coin_info": parent, "puzzle_hash": "22" * 32, "amount": 5000},
                cs.coin_id(parent, "22" * 32, 5000))

    x, xid = coin("d4" * 32)
    y, yid = coin("d5" * 32)
    store.update_job(job.id, state_patch={
        "burn_tx_id": S._FUNDING_TX_UNKNOWN, "burn_tx_id_removals": [yid],
        "burn_cat_coin_id": None,
        "burn_tx_id_prior": [k1], "burn_tx_id_prior_removals": {k1: [xid]},
        "burn_tx_id_prior_dead": [k1]})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[xid] = _rec(spent=True)              # A dead
    ctx.coinset.records[yid] = _rec(spent=False)             # B could still confirm
    ctx.wallet.cat_txs = [{"name": "old-burn", "removals": [x], "additions": [
        {"puzzle_hash": burn_ph, "amount": AMOUNT, "parent_coin_info": "d4" * 32}]}]
    out = engine.step()
    assert out["status"] == JobStatus.BURN_SENT
    st = store.get_active_job().state
    assert st["burn_tx_id"] == S._FUNDING_TX_UNKNOWN, "A's record is not B"
    assert st["burn_tx_id_removals"] == [yid]
    assert ctx.wallet.cat_spends == []

    ctx.wallet.cat_txs.append({"name": "new-burn", "removals": [y], "additions": [
        {"puzzle_hash": burn_ph, "amount": AMOUNT, "parent_coin_info": "d5" * 32}]})
    engine.step()
    st = store.get_active_job().state
    assert st["burn_tx_id"] == "new-burn"
    assert st["burn_tx_id_removals"] == [yid], "B's own evidence is kept"


def test_the_dedupe_scan_skips_a_retired_orphans_record(monkeypatch):
    """[review] After Retry retired an orphan, its record still in the
    history must not be adopted as "already in flight", or the replacement
    is never sent and Retry loops."""
    from gui.services.warp import chia_send as cs

    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    x = {"parent_coin_info": "d4" * 32, "puzzle_hash": "11" * 32, "amount": 499_005_000_000}
    xid = cs.coin_id("d4" * 32, "11" * 32, 499_005_000_000)
    job = _seed_funding(store, funding_tx_id=S._FUNDING_TX_UNKNOWN,
                        funding_amount=EXPECTED, funding_tx_id_removals=[xid])
    ctx.coinset.records[xid] = _rec(spent=False)             # an orphan
    ctx.wallet.txs = [{"name": "orphan-rec", "removals": [x], "additions": [
        {"puzzle_hash": SECURITY_PH.hex(), "amount": EXPECTED}]}]
    ctx.wallet.tx_records["orphan-rec"] = {"confirmed": False, "removals": [x]}
    # Not adopted while it is the current attempt either: its record IS the
    # current attempt (same coins), so it is adopted by name...
    engine.step()
    assert store.get_active_job().state["funding_tx_id"] == "orphan-rec"
    # ...and once Retry retires it, the record is nobody's to adopt.
    ctx.wallet.get_transaction_or_none = lambda tid: None     # gone from the wallet
    store.update_job(job.id, status=JobStatus.FAILED,
                     expected_status=JobStatus.FUNDING_CLAIM,
                     state_patch={"failed_from": JobStatus.FUNDING_CLAIM})
    engine.job_action(job.id, "retry")
    assert store.get_job(job.id).state["funding_tx_id"] is None
    engine.step()
    assert len(ctx.wallet.sent) == 1, "the replacement is sent"
    assert ctx.wallet.sent_excludes[-1] == [xid]


# --------------------------------------------------------------------------- #
# Independent review (own pass before Copilot).
# --------------------------------------------------------------------------- #

def test_an_unobservable_retired_attempt_is_presumed_gone_after_the_grace(monkeypatch):
    """A retired send with no coins on record, no wallet record and no
    mempool item can never be proven anything; it keeps the job open for the
    grace, then settles as gone -- never for good."""
    monkeypatch.setattr(S, "_UNOBSERVABLE_ATTEMPT_GRACE_S", 6 * 3600)
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    now = 1_000_000.0
    monkeypatch.setattr(engine, "_now", lambda: now)          # a still clock
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": ["toll-a"], "funding_tx_id_prior_removals": None,
        "funding_tx_id_prior_retired_at": {"toll-a": now - 3600.0}})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID2] = _rec(spent=True)             # the current attempt: dead
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "presumed gone after 6h" in note
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior_retired_at": {"toll-a": now - 7 * 3600.0}})
    assert engine._prior_sends_settled(store.get_active_job())[0]

    # An unnamed retired attempt with no coins and no history: the same clock,
    # started the first time it is seen.
    k1 = S._FUNDING_TX_UNKNOWN + "#1"
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": [k1], "funding_tx_id_prior_retired_at": None})
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "presumed gone" in note
    stamped = store.get_job(job.id).state["funding_tx_id_prior_retired_at"]
    assert k1 in stamped
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior_retired_at": {k1: now - 7 * 3600.0}})
    assert engine._prior_sends_settled(store.get_active_job())[0]


def test_the_current_attempt_never_settles_by_time_alone():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    store.update_job(job.id, state_patch={
        "funding_tx_id": S._FUNDING_TX_UNKNOWN, "funding_tx_id_removals": None,
        "funding_tx_id_prior_retired_at": {
            S._FUNDING_TX_UNKNOWN + "#current": engine._now() - 48 * 3600.0}})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "not known" in note


def test_retirement_is_stamped(monkeypatch):
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding(store, funding_tx_id=DEAD_TX, funding_amount=EXPECTED,
                  funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK
    monkeypatch.setattr(engine, "_now", lambda: 1_000_000.0)
    engine.step()
    stamps = store.get_active_job().state["funding_tx_id_prior_retired_at"]
    assert stamps[DEAD_TX] == 1_000_000.0


def test_a_connection_never_made_releases_the_reservation(monkeypatch):
    """No bytes reached the daemon: nothing was sent, and the next tick may
    send without first waiting on an attempt nothing can ever rule out."""
    from gui.services.warp.wallet import WalletError

    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store)
    calls = []

    def refused(*a, **k):
        calls.append(1)
        if len(calls) == 1:
            raise WalletError("send_transaction request failed: HTTPSConnectionPool: "
                              "Max retries exceeded (NewConnectionError: [WinError "
                              "10061] No connection could be made because the "
                              "target machine actively refused it)")
        return {"name": "funding-tx-2"}

    ctx.wallet.send_transaction = refused
    engine.step()
    assert store.get_job(job.id).state.get("funding_tx_id") is None
    store.update_job(job.id, columns={"next_retry_at": None})
    engine.step()
    assert store.get_active_job().state["funding_tx_id"] == "funding-tx-2"


def test_abandon_records_a_completed_claim_or_delivery_truthfully():
    store = new_store()
    engine, ctx = build(store)
    job = seed(store, JobStatus.FAILED,
               columns={"bridge_nonce": "cd" * 32, "post_tip_mojos": 4985,
                        "receiver_ph": "ab" * 32},
               state={"failed_from": JobStatus.CLAIMING, "final_cat_coin_id": "f2" * 32,
                      "funding_tx_id_prior": ["fund-a", "fund-b"],
                      "funding_tx_id_prior_dead": ["fund-b"]})
    ctx.coinset.records["f2" * 32] = _rec(spent=False)       # the claim landed
    out = engine.job_action(job.id, "abandon")
    assert out["status"] == JobStatus.CANCELLED
    note = store.get_events(job.id, limit=5000)[-1].message
    assert "the claim completed" in note and "never claimed" not in note
    assert "1 replaced funding send(s) not ruled out: fund-a" in note

    job2 = seed(store, JobStatus.FAILED,
                columns={"bridge_nonce": "ce" * 32, "amount_mojos": AMOUNT},
                state={"direction": "out", "failed_from": JobStatus.RELAYING,
                       "delivered_by": "our relay", "receiver_evm": "ab" * 20})
    engine.job_action(job2.id, "abandon")
    note2 = store.get_events(job2.id, limit=5000)[-1].message
    assert "the unwrap was delivered (our relay)" in note2 and "never claimed" not in note2


def test_a_failed_history_lookup_never_starts_or_advances_the_grace(monkeypatch):
    """[review] "Could not look" is not "absent": an unavailable wallet
    history keeps the attempt unresolved and leaves the grace clock alone."""
    k1 = S._FUNDING_TX_UNKNOWN + "#1"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    now = 1_000_000.0
    monkeypatch.setattr(engine, "_now", lambda: now)
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": [k1], "funding_tx_id_prior_removals": None,
        "funding_tx_id_prior_retired_at": None})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID2] = _rec(spent=True)

    def down(*a, **k):
        raise RuntimeError("wallet rpc down")

    ctx.wallet.get_transactions = down
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "could not be read" in note
    assert not store.get_job(job.id).state.get("funding_tx_id_prior_retired_at"), \
        "no clock started on an unanswered lookup"
    # Even past the grace, an unanswered lookup settles nothing.
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior_retired_at": {k1: now - 48 * 3600.0}})
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "could not be read" in note


def test_the_worker_follows_a_config_switch(tmp_path):
    """[review] A switched config moves the worker's job-store anchor with
    it and rebuilds the engine there, as the C++ engine will read it."""
    closed = []
    worker = S._WarpWorker(secrets_path=tmp_path / "one" / "secrets.yaml")
    worker._engine = SimpleNamespace(close=lambda: closed.append(1))
    assert worker._config_home() == tmp_path / "one"
    worker.set_secrets_path(tmp_path / "two" / "secrets.yaml")
    assert worker._config_home() == tmp_path / "two"
    assert closed == [1] and worker._engine is None
    worker.set_secrets_path(tmp_path / "two" / "secrets.yaml")   # unchanged: no-op
    assert closed == [1]


def test_the_sweep_destination_is_pinned_per_coin_across_rebuilds(monkeypatch):
    """[review] The bundle is (coin, destination, fee): a destination
    re-resolved after a rebuild would make a conflicting twin, so the first
    one is persisted and reused."""
    from gui.services.warp import clvm_utils as cu

    seen = []

    def push(coinset, net, *, security_coin, destination_puzzle_hash,
             ephemeral_sk, sweep_fee=0):
        seen.append(bytes(destination_puzzle_hash).hex())
        return ("accepted", "ok")

    monkeypatch.setattr(claim_mod, "build_and_push_sweep", push)
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: False)
    store = new_store()
    engine, ctx = build(store, params=default_params(offer_fee_mojos=10_000_000))
    job = _seed_funding(store, funding_amount=EXPECTED)
    engine._sweep_security(job, FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    first = seen[-1]
    assert store.get_job(job.id).state["sweep_destination_by_coin"] == {COIN: first}

    # A rebuilt engine whose wallet now hands out a later receive address...
    engine._sweep_dest_cache = None
    ctx.wallet.next_address = cu.encode_puzzle_hash(b"\x66" * 32, NET.chia_prefix)
    engine._sweep_security(store.get_job(job.id), FakeCoin(b"\x5a" * 32), b"\x01" * 32)
    assert seen[-1] == first, "...replays the SAME bundle, not a conflicting twin"
    # ...while another coin of the same job gets its own destination.
    engine._sweep_security(store.get_job(job.id), FakeCoin(b"\x5c" * 32), b"\x01" * 32)
    assert seen[-1] == (b"\x66" * 32).hex()


def test_an_identified_retired_attempt_is_persisted_under_its_real_id(monkeypatch):
    """[review] Once the history names a retired unnamed attempt, its id and
    coins are kept: a record that later drops out of the history is judged
    by its coins (the bundle may still sit in a mempool), never presumed
    gone by absence."""
    from gui.services.warp import chia_send as cs

    k1 = S._FUNDING_TX_UNKNOWN + "#1"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    ph = claim_mod.security_coin_puzzle_hash(engine._load_ephemeral_sk(job)).hex()
    now = 1_000_000.0
    monkeypatch.setattr(engine, "_now", lambda: now)
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": [k1], "funding_tx_id_prior_removals": None,
        "funding_tx_id_prior_retired_at": {k1: now - 10 * 3600.0}})
    removal = {"parent_coin_info": "d4" * 32, "puzzle_hash": "11" * 32,
               "amount": 499_005_000_000}
    rid = cs.coin_id("d4" * 32, "11" * 32, 499_005_000_000)
    ctx.wallet.txs = [{"name": "toll-x", "confirmed": False, "removals": [removal],
                       "additions": [{"puzzle_hash": ph, "amount": NET.chia_toll_mojos}]}]
    ctx.wallet.tx_records["toll-x"] = {"confirmed": False, "removals": [removal]}
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID2] = _rec(spent=True)
    ctx.coinset.records[rid] = _rec(spent=False)             # toll-x could still land
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "could still land" in note
    st = store.get_job(job.id).state
    assert st["funding_tx_id_prior"] == ["toll-x"]
    assert st["funding_tx_id_prior_removals"] == {"toll-x": [rid]}
    assert "toll-x" in st["funding_tx_id_prior_retired_at"] and k1 not in st["funding_tx_id_prior_retired_at"]

    # The record drops out of the wallet history and the wallet: judged by
    # its coins, still unspent -- not "unobservable", not presumed gone.
    ctx.wallet.txs = []
    ctx.wallet.tx_records.clear()
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "could still land" in note
    ctx.coinset.records[rid] = _rec(spent=True)              # ...until proven dead
    assert engine._prior_sends_settled(store.get_active_job())[0]


# --------------------------------------------------------------------------- #
# Review round 32.
# --------------------------------------------------------------------------- #

def test_a_retired_attempts_key_is_never_one_in_use_or_ever_used():
    """[review] After #1 was re-keyed under its real id while #2 is still
    outstanding, the next retired attempt must not be "#2" again."""
    k2 = S._FUNDING_TX_UNKNOWN + "#2"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": ["toll-x", k2],
        "funding_tx_id_prior_removals": {"toll-x": [RID], k2: None}})
    assert engine._unnamed_attempt_key(store.get_active_job(), "funding_tx_id") \
        == S._FUNDING_TX_UNKNOWN + "#3"
    # A re-keyed "#3" leaves no trace in the books but the sequence remembers it.
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": ["toll-x", "toll-y"], "funding_tx_id_unnamed_seq": 3})
    assert engine._unnamed_attempt_key(store.get_active_job(), "funding_tx_id") \
        == S._FUNDING_TX_UNKNOWN + "#4"


def test_the_sequence_of_unnamed_keys_is_persisted_on_retirement(monkeypatch):
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_tx_id=S._FUNDING_TX_UNKNOWN,
                        funding_amount=EXPECTED, funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK
    engine.step()                                            # dead: retired as #1
    st = store.get_active_job().state
    assert st["funding_tx_id_prior"] == [S._FUNDING_TX_UNKNOWN + "#1"]
    assert st["funding_tx_id_unnamed_seq"] == 1


def test_two_identifications_in_one_pass_are_both_kept():
    """[review] Each re-key builds on the previous write, not on the
    caller's stale snapshot."""
    from gui.services.warp import chia_send as cs

    k1, k2 = S._FUNDING_TX_UNKNOWN + "#1", S._FUNDING_TX_UNKNOWN + "#2"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    ph = claim_mod.security_coin_puzzle_hash(engine._load_ephemeral_sk(job)).hex()
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": [k1, k2], "funding_tx_id_prior_removals": None})

    def tx(name, parent):
        removal = {"parent_coin_info": parent, "puzzle_hash": "11" * 32,
                   "amount": 499_005_000_000}
        rid = cs.coin_id(parent, "11" * 32, 499_005_000_000)
        ctx.wallet.tx_records[name] = {"confirmed": False, "removals": [removal]}
        ctx.coinset.records[rid] = _rec(spent=True)          # both proven dead
        return rid, {"name": name, "confirmed": False, "removals": [removal],
                     "additions": [{"puzzle_hash": ph, "amount": NET.chia_toll_mojos}]}

    xid, tx1 = tx("toll-x", "d4" * 32)
    yid, tx2 = tx("toll-y", "d5" * 32)
    ctx.wallet.txs = [tx1, tx2]
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID2] = _rec(spent=True)
    assert engine._prior_sends_settled(store.get_active_job())[0]
    st = store.get_job(job.id).state
    assert st["funding_tx_id_prior"] == ["toll-x", "toll-y"]
    assert st["funding_tx_id_prior_removals"] == {"toll-x": [xid], "toll-y": [yid]}


def test_the_burn_bundle_spends_the_pinned_cat_not_the_first_match(monkeypatch):
    """[review] burn_cat_ph is shared by every unwrap to one receiver: the
    bundle must spend the coin BURN_SENT pinned, never an unrelated
    same-amount coin that happens to be listed first."""
    from gui.services.warp import clvm_utils as cu, drivers
    from .test_warp_service import seed, make_ephemeral_blob

    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    ours_parent, other_parent = "dd" * 32, "cc" * 32
    pinned_id = cu.coin_name(bytes.fromhex(ours_parent), bytes.fromhex("bb" * 32), AMOUNT).hex()
    job = seed(store, JobStatus.BURNING, columns={"amount_mojos": AMOUNT},
               state={"direction": "out", "receiver_evm": "ab" * 20,
                      "burn_cat_ph": "bb" * 32, "cat_wallet_id": 3,
                      "burn_cat_coin_id": pinned_id, "burn_cat_parent": ours_parent})
    blob, _sk = make_ephemeral_blob(job.id)
    sec = drivers.Coin(b"\x5a" * 32, b"\x5b" * 32, NET.chia_toll_mojos)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob,
                                          "security_coin_id": sec.name().hex()})
    sec_record = SimpleNamespace(
        coin=SimpleNamespace(parent_coin_info=(b"\x5a" * 32).hex(),
                             puzzle_hash=(b"\x5b" * 32).hex(), amount=NET.chia_toll_mojos),
        spent=False, confirmed_block_index=100, spent_block_index=0)
    ctx.coinset.records[sec.name().hex()] = sec_record

    def cat(parent):
        return SimpleNamespace(coin=SimpleNamespace(parent_coin_info=parent,
                                                    puzzle_hash="bb" * 32, amount=AMOUNT),
                               spent=False, confirmed_block_index=100, spent_block_index=0)

    ctx.coinset.by_ph = [cat(other_parent), cat(ours_parent)]      # the stranger first
    monkeypatch.setattr(claim_mod, "cat_lineage_proof_for",
                        lambda coinset, parent: [b"\x33" * 32, b"\x44" * 32, AMOUNT])
    built = {}

    def capture(req):
        built["cat"] = req.burn_cat_coin
        raise S.WarpPending("stop here")

    monkeypatch.setattr(drivers, "build_burn_bundle", capture)
    engine.step()
    assert built["cat"].parent_coin_info.hex() == ours_parent

    # Pinned but not yet indexed anywhere: wait, never substitute.
    built.clear()
    ctx.coinset.by_ph = [cat(other_parent)]
    store.update_job(job.id, columns={"next_retry_at": None})
    out = engine.step()
    assert not built and "not indexed yet" in (out.get("last_error") or _last_message(store, job.id))


def test_learned_removals_of_a_named_retired_attempt_are_persisted(monkeypatch):
    """[review] A retired send recorded without coins whose wallet record
    reappears teaches its coins; they stay in its book, so when the record
    vanishes again the send is judged by them and never presumed gone."""
    from gui.services.warp import chia_send as cs

    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    now = 1_000_000.0
    monkeypatch.setattr(engine, "_now", lambda: now)
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": ["toll-a"], "funding_tx_id_prior_removals": None,
        "funding_tx_id_prior_retired_at": {"toll-a": now - 10 * 3600.0}})
    removal = {"parent_coin_info": "d4" * 32, "puzzle_hash": "11" * 32,
               "amount": 499_005_000_000}
    rid = cs.coin_id("d4" * 32, "11" * 32, 499_005_000_000)
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID2] = _rec(spent=True)
    ctx.coinset.records[rid] = _rec(spent=False)
    ctx.wallet.tx_records["toll-a"] = {"confirmed": False, "removals": [removal]}
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "could still land" in note
    assert store.get_job(job.id).state["funding_tx_id_prior_removals"] == {"toll-a": [rid]}

    ctx.wallet.tx_records.clear()                            # the record vanishes again
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "could still land" in note and "no coins on record" not in note
    ctx.coinset.records[rid] = _rec(spent=True)              # ...until proven dead
    assert engine._prior_sends_settled(store.get_active_job())[0]


def test_an_attempt_identified_without_coins_is_probed_before_any_absence_rule(monkeypatch):
    """[review] The history names the send but its record vanished before
    the by-id lookup: the id is kept and probed next pass -- a bundle still
    in the mempool keeps the job open however old the retirement is."""
    monkeypatch.setattr(S, "_UNOBSERVABLE_ATTEMPT_GRACE_S", 6 * 3600)
    k1 = S._FUNDING_TX_UNKNOWN + "#1"
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    ph = claim_mod.security_coin_puzzle_hash(engine._load_ephemeral_sk(job)).hex()
    now = 1_000_000.0
    monkeypatch.setattr(engine, "_now", lambda: now)
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior": [k1], "funding_tx_id_prior_removals": None,
        "funding_tx_id_prior_retired_at": {k1: now - 10 * 3600.0}})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID2] = _rec(spent=True)
    # The history lists toll-x with no removals and the by-id lookup finds nothing.
    ctx.wallet.txs = [{"name": "toll-x", "confirmed": False,
                       "additions": [{"puzzle_hash": ph, "amount": NET.chia_toll_mojos}]}]
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "identified as toll-x" in note
    st = store.get_job(job.id).state
    assert st["funding_tx_id_prior"] == ["toll-x"]

    ctx.coinset.mempool["toll-x"] = {"removals": []}          # the bundle is in a mempool
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "mempool" in note and "presumed gone" not in note

    del ctx.coinset.mempool["toll-x"]                        # nowhere, no coins: grace (10h)
    ctx.wallet.txs = []
    assert engine._prior_sends_settled(store.get_active_job())[0]


# --------------------------------------------------------------------------- #
# Second independent review.
# --------------------------------------------------------------------------- #

def test_an_orphaned_retired_attempt_with_coins_settles_after_the_grace(monkeypatch):
    """A retired send whose coins are known but unspent, absent from wallet
    and mempool: bounded like the unobservable case, and recorded as
    presumed gone so the engine guard and the exclusions release it."""
    monkeypatch.setattr(S, "_UNOBSERVABLE_ATTEMPT_GRACE_S", 6 * 3600)
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    now = 1_000_000.0
    monkeypatch.setattr(engine, "_now", lambda: now)
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior_retired_at": {"toll-a": now - 3600.0}})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID2] = _rec(spent=True)
    ctx.coinset.records[RID] = _rec(spent=False)             # toll-a: an orphan
    settled, note = engine._prior_sends_settled(store.get_active_job())
    assert not settled and "its coins are unspent" in note and "presumed gone after 6h" in note
    store.update_job(job.id, state_patch={
        "funding_tx_id_prior_retired_at": {"toll-a": now - 7 * 3600.0}})
    assert engine._prior_sends_settled(store.get_active_job())[0]
    assert store.get_job(job.id).state["funding_tx_id_prior_dead"] == ["toll-a"]


def test_dead_verdicts_found_during_settlement_are_persisted():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = _job_with_a_landed_replaced_toll(store)
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)              # toll-a: dead
    ctx.coinset.records[RID2] = _rec(spent=True)
    assert engine._prior_sends_settled(store.get_active_job())[0]
    assert store.get_job(job.id).state["funding_tx_id_prior_dead"] == ["toll-a"]


def test_the_claim_prefers_the_pinned_security_coin():
    """Two coins at the key (a replaced funding send landed beside its
    replacement): the claim is built around the pinned coin, whatever the
    coinset lists first."""
    from gui.services.warp import drivers

    store = new_store()
    engine, ctx = build(store)
    job = seed(store, JobStatus.CLAIM_FUNDED,
               columns={"receiver_ph": "ab" * 32, "post_tip_mojos": 4985})
    blob, _sk = make_ephemeral_blob(job.id)
    sk = engine._load_ephemeral_sk(store.get_active_job()) if False else None
    ours = drivers.Coin(b"\x5a" * 32, b"\x5b" * 32, EXPECTED)
    other = drivers.Coin(b"\x6a" * 32, b"\x5b" * 32, EXPECTED)
    store.update_job(job.id, state_patch={
        "ephemeral_blob": blob, "funding_amount": EXPECTED,
        "security_coin_id": ours.name().hex()})

    def rec(coin, spent=False):
        return SimpleNamespace(
            coin=SimpleNamespace(parent_coin_info=coin.parent_coin_info.hex(),
                                 puzzle_hash=coin.puzzle_hash.hex(), amount=coin.amount),
            spent=spent, confirmed_block_index=PEAK - 100, spent_block_index=0)

    ctx.coinset.records[ours.name().hex()] = rec(ours)
    ctx.coinset.records[other.name().hex()] = rec(other)
    ctx.coinset.by_ph = [rec(other), rec(ours)]                 # the other first
    ctx.coinset.peak = PEAK
    out = engine.step()
    assert out["status"] == JobStatus.COLLECTING_SIGS
    assert store.get_active_job().state["security_coin_id"] == ours.name().hex()
    # Spent: the callers read None as "awaiting the final CAT coin".
    ctx.coinset.records[ours.name().hex()] = rec(ours, spent=True)
    assert engine._security_coin_for(store.get_active_job(), b"\x01" * 32, EXPECTED) is None


def test_burning_waits_for_an_unindexed_pinned_toll_coin(monkeypatch):
    """A pinned toll coin the index does not show yet is awaited, never
    replaced by another coin of the amount at the key."""
    from gui.services.warp import drivers
    from .test_warp_service import seed, make_ephemeral_blob

    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    job = seed(store, JobStatus.BURNING, columns={"amount_mojos": AMOUNT},
               state={"direction": "out", "receiver_evm": "ab" * 20,
                      "burn_cat_ph": "bb" * 32, "cat_wallet_id": 3})
    blob, _sk = make_ephemeral_blob(job.id)
    pinned = drivers.Coin(b"\x5a" * 32, b"\x5b" * 32, NET.chia_toll_mojos)
    store.update_job(job.id, state_patch={
        "ephemeral_blob": blob, "security_coin_id": pinned.name().hex(),
        "unwrap_nonce": "cd" * 32})
    other = drivers.Coin(b"\x6a" * 32, b"\x5b" * 32, NET.chia_toll_mojos)
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: other)
    built = []
    monkeypatch.setattr(drivers, "build_burn_bundle",
                        lambda req: built.append(req) or (_ for _ in ()).throw(S.WarpPending("x")))
    out = engine.step()
    assert out["status"] == JobStatus.BURNING and built == []
    assert "not indexed yet" in _last_message(store, job.id)


def test_a_pinned_security_coin_never_yields_to_another_coin_at_the_key(monkeypatch):
    """[review] A pinned coin the by-name index does not show yet is found by
    its computed name among the key's records, or awaited -- never replaced
    by another coin of the amount."""
    from gui.services.warp import drivers

    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_amount=EXPECTED)
    sk = engine._load_ephemeral_sk(job)
    ph = claim_mod.security_coin_puzzle_hash(sk)
    ours = drivers.Coin(b"\x5a" * 32, ph, EXPECTED)
    other = drivers.Coin(b"\x6a" * 32, ph, EXPECTED)
    store.update_job(job.id, state_patch={"security_coin_id": ours.name().hex()})
    job = store.get_active_job()

    def rec(coin, spent=False):
        return SimpleNamespace(
            coin=SimpleNamespace(parent_coin_info=coin.parent_coin_info.hex(),
                                 puzzle_hash=coin.puzzle_hash.hex(), amount=coin.amount),
            spent=spent, confirmed_block_index=PEAK - 100, spent_block_index=0)

    ctx.coinset.by_ph = [rec(other)]                          # only the other is indexed
    assert engine._security_coin_for(job, sk, EXPECTED) is None, "awaited, not re-elected"
    ctx.coinset.by_ph = [rec(other), rec(ours)]               # ours by its computed name
    assert engine._security_coin_for(job, sk, EXPECTED).name() == ours.name()
    ctx.coinset.by_ph = [rec(other), rec(ours, spent=True)]
    assert engine._security_coin_for(job, sk, EXPECTED) is None
    # No pin at all: the key scan as before.
    store.update_job(job.id, state_patch={"security_coin_id": None})
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: other)
    assert engine._security_coin_for(store.get_active_job(), sk, EXPECTED) is other


def test_a_confirmed_third_party_sweep_resumes_recovery(monkeypatch):
    """[review] The recovery sweep was pending when the tick ended; once the
    security coin is spent the job resumes settlement and completes instead
    of awaiting a final CAT coin that can never appear."""
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    store = new_store()
    engine, ctx = build(store)
    job = seed(store, JobStatus.CLAIMING,
               columns={"receiver_ph": "ab" * 32, "post_tip_mojos": 4985,
                        "bridge_nonce": "cd" * 32},
               state={"message_destination": "ee" * 32, "third_party_claim": True,
                      "sweep_status": "pending", "security_coin_id": "77" * 32,
                      "funding_tx_id": "fund-b", "funding_tx_id_removals": [RID2],
                      "funding_tx_id_prior": ["fund-a"],
                      "funding_tx_id_prior_removals": {"fund-a": [RID]}})
    blob, _sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob, "funding_amount": EXPECTED})
    ctx.coinset.peak = PEAK
    ctx.coinset.records[RID] = _rec(spent=True)              # fund-a: dead
    ctx.coinset.records[RID2] = _rec(spent=True)             # fund-b: landed, swept at depth
    ctx.coinset.by_ph = [SimpleNamespace(
        coin=SimpleNamespace(parent_coin_info=RID2, puzzle_hash="5b" * 32, amount=EXPECTED),
        spent=True, confirmed_block_index=PEAK - 50, spent_block_index=PEAK - 40)]
    out = engine.step()
    assert out["status"] == JobStatus.COMPLETED
    assert store.get_job(job.id).state["swept"] is True


def test_a_pending_third_party_sweep_is_pushed_again(monkeypatch):
    pushes = []
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: False)
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: pushes.append(1) or ("pending", "already in mempool"))
    store = new_store()
    engine, ctx = build(store)
    job = seed(store, JobStatus.CLAIMING,
               columns={"receiver_ph": "ab" * 32, "post_tip_mojos": 4985,
                        "bridge_nonce": "cd" * 32},
               state={"message_destination": "ee" * 32, "third_party_claim": True,
                      "sweep_status": "pending"})
    blob, _sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob, "funding_amount": EXPECTED})
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    out = engine.step()
    assert out["status"] == JobStatus.CLAIMING and pushes == [1]
    assert "retrying" in _last_message(store, job.id)


# --------------------------------------------------------------------------- #
# Third independent review: what the operator can see.
# --------------------------------------------------------------------------- #

def test_the_job_view_carries_the_sweep_outcome_and_the_open_attempts():
    store = new_store()
    job = seed(store, JobStatus.FAILED, state={
        "failed_from": JobStatus.FUNDING_CLAIM, "swept": False,
        "sweep_status": "ok; attempt fund-a... could still land",
        "funding_tx_id_prior": ["fund-a", "fund-b"], "funding_tx_id_prior_dead": ["fund-b"],
        "burn_tx_id_prior": ["burn-a"]})
    view = S._job_dict(store.get_job(job.id))
    assert view["swept"] is False and "could still land" in view["sweep_status"]
    assert view["attempts_open"] == 2


def test_a_sweep_that_does_not_close_the_job_is_a_notice_in_the_snapshot(monkeypatch):
    """[review] The operator saw a click do nothing."""
    worker = S._WarpWorker()
    worker._engine = SimpleNamespace(
        job_action=lambda jid, action: {"status": JobStatus.FAILED,
                                        "sweep_status": "ok; Sweep again later"},
        snapshot=lambda: {"enabled": True})
    monkeypatch.setattr(worker, "_ensure_engine", lambda: worker._engine)
    monkeypatch.setattr(worker, "_wallet_summary", lambda hot: {})
    snap = worker._guarded(lambda e: e.job_action(1, "sweep"))
    assert snap["action_notice"] == "Sweep: ok; Sweep again later"
    assert "action_error" not in snap


def test_the_no_coin_sweep_status_names_the_sends_fate(monkeypatch):
    """[review] Job 4's pre-Retry shape used to read "may not be indexed yet"."""
    _no_coin(monkeypatch)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_funding(store, funding_tx_id=DEAD_TX, funding_amount=EXPECTED,
                        funding_tx_id_removals=[RID])
    ctx.coinset.records[RID] = _rec(spent=True)
    ctx.coinset.peak = PEAK
    resolved, status = engine._funding_provably_gone(job)
    assert not resolved and "can never confirm" in status and "Retry sends it again" in status
    ctx.coinset.records[RID] = _rec(spent=False)
    resolved, status = engine._funding_provably_gone(job)
    assert not resolved and "Retry sends it again" in status and "excluded" in status


def test_a_completed_claims_deadline_names_the_funding_wait():
    store = new_store()
    engine, ctx = build(store)
    limit = S._PENDING_MAX_POLLS[JobStatus.CLAIMING]
    ctx.coinset.records["f2" * 32] = _rec(spent=False)        # the claim landed
    reason = engine._pending_expired(SimpleNamespace(
        status=JobStatus.CLAIMING,
        state={"pending_polls": limit, "final_cat_coin_id": "f2" * 32}))
    assert "the claim completed" in reason and "Sweep recovers a coin that landed" in reason
    plain = engine._pending_expired(SimpleNamespace(
        status=JobStatus.CLAIMING, state={"pending_polls": limit}))
    assert "the claim completed" not in plain
