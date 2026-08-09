"""Regression tests for the review findings on the warp bridge PR.

Every test here fails against the implementation as originally submitted. They
are grouped by finding so a failure names the bug it protects rather than just
the behaviour it asserts:

* **F1** -- testnet is gone; ``warp.dry_run`` replaces it as the rehearsal gate.
* **F2** -- ``min_auto_bridge_usdc`` is an auto-bridge floor only, so a small
  manual test deposit is not stuck behind the 100-USDC default.
* **F3** -- a failed wallet-history scan is never read as "not funded yet",
  which is what let a lost send response fund the claim coin twice.
* **F4/F5** -- a resolved Sweep closes a FAILED job and frees the single
  active-job slot; the widget and the engine now agree on who owns it.
* **F6** -- the effective (wallet-derived) receiver is resolved once, cached,
  and published to the GUI.
* **F7** -- third-party-claim detection is per-nonce, so the bot's own wUSDC.b
  trading cannot mark an unclaimed bridge COMPLETED.
* **F8** -- a job frozen against one hot wallet is never resumed under another.
* **F10** -- the wrapped-asset anchor runs at engine construction, before any
  client exists.

The fakes, builders and constants are shared with :mod:`test_warp_service`
rather than duplicated, so the two modules cannot drift.
"""

from __future__ import annotations

from types import SimpleNamespace

import pytest

pytest.importorskip("clvm")
pytest.importorskip("chia_rs")

from gui.services.warp import claim as claim_mod  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import drivers as drivers_mod  # noqa: E402
from gui.services.warp import evm as evm_mod  # noqa: E402
from gui.services.warp import service as S  # noqa: E402
from gui.services.warp.jobs import JobStatus  # noqa: E402

from .test_warp_service import (  # noqa: E402
    ADDR,
    FINAL_CAT,
    NET,
    PORTAL,
    RECEIVER_PH,
    FakeCoin,
    build,
    default_params,
    fake_sign_tx,
    make_ephemeral_blob,
    message_sent_receipt,
    new_store,
    seed,
)

HOT = "ab" * 20  # the address behind build()'s fake EVM key
OTHER_HOT = "cd" * 20


# --------------------------------------------------------------------------- #
# F2 -- manual bridges ignore the auto floor, honour the cap.
# --------------------------------------------------------------------------- #

def test_manual_bridge_ignores_the_auto_min_floor():
    """A ~$5 manual test must not hang behind min_auto_bridge_usdc: 100."""
    store = new_store()
    engine, ctx = build(store, params=default_params(min_micros=100_000_000))
    ctx.evm.erc20 = 5_000_000  # $5, far below the $100 floor
    seed(store, JobStatus.AWAITING_DEPOSIT, state={"manual": True})

    out = engine.step()

    assert out["status"] == JobStatus.DEPOSIT_SEEN
    assert store.get_active_job().amount_usdc_micros == 5_000_000


def test_auto_bridge_still_honours_the_min_floor():
    store = new_store()
    engine, ctx = build(store, params=default_params(min_micros=100_000_000))
    ctx.evm.erc20 = 5_000_000
    seed(store, JobStatus.AWAITING_DEPOSIT, state={"auto": True})

    out = engine.step()

    assert out["status"] == JobStatus.AWAITING_DEPOSIT
    assert store.get_active_job().retry_count == 0  # pending, not an error


def test_unflagged_legacy_job_is_treated_as_automatic():
    """No manual flag -> the conservative side, i.e. the floor applies."""
    store = new_store()
    engine, ctx = build(store, params=default_params(min_micros=100_000_000))
    ctx.evm.erc20 = 5_000_000
    seed(store, JobStatus.AWAITING_DEPOSIT)

    assert engine.step()["status"] == JobStatus.AWAITING_DEPOSIT


def test_manual_bridge_is_still_clamped_by_the_max_cap():
    """max_micros is the blast radius; it binds manual clicks too."""
    store = new_store()
    engine, ctx = build(store, params=default_params(min_micros=0, max_micros=5_000_000))
    ctx.evm.erc20 = 900_000_000  # $900 sitting in the hot wallet
    seed(store, JobStatus.AWAITING_DEPOSIT, state={"manual": True})

    out = engine.step()

    assert out["status"] == JobStatus.DEPOSIT_SEEN
    assert store.get_active_job().amount_usdc_micros == 5_000_000


# --------------------------------------------------------------------------- #
# F3 -- the funding dedupe scan fails closed.
# --------------------------------------------------------------------------- #

def _seed_funding_claim(store):
    job = seed(
        store, JobStatus.FUNDING_CLAIM,
        columns={"receiver_ph": RECEIVER_PH.hex(), "post_tip_mojos": 4985},
    )
    blob, sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob})
    return store.get_active_job(), sk


def test_funding_claim_refuses_to_send_when_the_history_scan_fails(monkeypatch):
    """A lost send response plus an unindexed coin must not fund twice."""
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    monkeypatch.setattr(claim_mod, "security_coin_puzzle_hash", lambda sk: b"\x5b" * 32)
    store = new_store()
    engine, ctx = build(store)

    def boom(*a, **k):
        raise TimeoutError("wallet RPC timed out")

    ctx.wallet.get_transactions = boom
    _seed_funding_claim(store)

    out = engine.step()

    assert ctx.wallet.sent == []                    # the actual guarantee
    assert out["status"] == JobStatus.FUNDING_CLAIM
    assert out["retry_count"] == 1                  # backed off, did not advance
    assert "refusing to send" in (out["last_error"] or "")


def test_funding_claim_honours_a_recorded_funding_tx(monkeypatch):
    """The persisted marker alone is enough to refuse a second send."""
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    store = new_store()
    engine, ctx = build(store)
    job, _ = _seed_funding_claim(store)
    store.update_job(job.id, state_patch={"funding_tx_id": "funding-tx-1"})

    out = engine.step()

    assert ctx.wallet.sent == []
    assert out["status"] == JobStatus.FUNDING_CLAIM


def test_funding_claim_sends_once_when_the_scan_is_clean(monkeypatch):
    """The happy path still funds -- fail-closed must not mean never-send."""
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    monkeypatch.setattr(claim_mod, "security_coin_puzzle_hash", lambda sk: b"\x5b" * 32)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding_claim(store)

    engine.step()

    assert len(ctx.wallet.sent) == 1
    assert store.get_active_job().state["funding_tx_id"] == "funding-tx-1"


def test_funding_claim_guards_even_when_the_wallet_names_no_transaction(monkeypatch):
    """[WARP-DUP-FUND] A send the wallet does not name must still block a resend.

    ``send_transaction`` returning neither ``name`` nor ``transaction_id``
    used to persist ``None``, which is falsy -- so the next tick sailed past
    the in-flight guard and funded the security coin a SECOND time out of the
    operator's wallet.  The dedupe scan cannot cover it: the coin is not on
    chain yet, which is the whole reason the guard exists.
    """
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    monkeypatch.setattr(claim_mod, "security_coin_puzzle_hash", lambda sk: b"\x5b" * 32)
    store = new_store()
    engine, ctx = build(store)
    _seed_funding_claim(store)

    def _unnamed_send(wallet_id, amount, address, fee_mojos=0):
        ctx.wallet.sent.append((amount, address, fee_mojos))
        return {"status": "SUCCESS"}          # accepted, but named nothing

    ctx.wallet.send_transaction = _unnamed_send

    engine.step()
    assert len(ctx.wallet.sent) == 1
    assert store.get_active_job().state["funding_tx_id"] == S._FUNDING_TX_UNKNOWN

    engine.step()
    assert len(ctx.wallet.sent) == 1, "the second tick must not re-fund the coin"


def test_funding_dedupe_hit_also_records_the_amount(monkeypatch):
    """The dedupe branch must persist funding_amount, not just the tx id.

    Without it a job resuming through the dedupe path leaves Sweep and
    _funding_provably_gone recomputing the expected amount from live config --
    the exact drift the funding_amount field was introduced to prevent.
    """
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    monkeypatch.setattr(claim_mod, "security_coin_puzzle_hash", lambda sk: b"\x5b" * 32)
    store = new_store()
    engine, ctx = build(store)
    job, _sk = _seed_funding_claim(store)
    expected = int(job.post_tip_mojos) + int(default_params().claim_fee_mojos)
    monkeypatch.setattr(
        S.WarpEngine, "_find_existing_funding", lambda self, ph, amt: "prior-tx"
    )

    engine.step()

    state = store.get_active_job().state
    assert ctx.wallet.sent == []
    assert state["funding_tx_id"] == "prior-tx"
    assert state["funding_amount"] == expected


# --------------------------------------------------------------------------- #
# F4 / F5 -- a resolved sweep frees the slot.
# --------------------------------------------------------------------------- #

def _seed_failed_funded(store, *, failed_from=JobStatus.CLAIMING):
    job = seed(store, JobStatus.FAILED,
               columns={"receiver_ph": RECEIVER_PH.hex(), "post_tip_mojos": 4985},
               state={"failed_from": failed_from})
    blob, _ = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob})
    return store.get_active_job()


def test_sweep_closes_a_failed_job_and_frees_the_slot(monkeypatch):
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: ("accepted", "ok"))
    # The sweep mines: the coin really is spent afterwards.
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    store = new_store()
    engine, _ = build(store)
    job = _seed_failed_funded(store)

    out = engine.job_action(job.id, "sweep")

    assert out["status"] == JobStatus.CANCELLED
    assert store.get_active_job() is None      # the whole point
    engine.request_bridge()                    # a new job is possible again


def test_a_state_that_never_progresses_eventually_reaches_the_operator(monkeypatch):
    """[WARP-PENDING-DEADLINE] An invisible pend is an unrecoverable job.

    _apply_stay resets retry_count and clears last_error on every pend --
    correctly, since nothing moved -- but that also made these loops
    unbounded. None of the pending states offers Cancel, Retry is a no-op on a
    non-FAILED job, and Sweep closes only FAILED, so a job stuck here held the
    single active slot forever. FAILED is the one status with affordances.
    """
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    monkeypatch.setattr(claim_mod, "security_coin_puzzle_hash", lambda sk: b"\x5b" * 32)
    monkeypatch.setitem(S._PENDING_MAX_POLLS, JobStatus.FUNDING_CLAIM, 3)
    store = new_store()
    engine, _ctx = build(store)
    job, _sk = _seed_funding_claim(store)
    store.update_job(job.id, state_patch={"funding_tx_id": "already-sent"})

    out = None
    for _ in range(6):
        out = engine.step()
        if out["status"] == JobStatus.FAILED:
            break

    assert out["status"] == JobStatus.FAILED
    assert "no progress in FUNDING_CLAIM" in (out["last_error"] or "")


def test_the_pending_counter_resets_when_the_job_advances(monkeypatch):
    """The clock measures time in one state, not the age of the job."""
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    store = new_store()
    engine, _ctx = build(store)
    job, _sk = _seed_funding_claim(store)
    store.update_job(job.id, state_patch={"pending_polls": 400})

    out = engine.step()          # the coin is there -> advance

    assert out["status"] == JobStatus.CLAIM_FUNDED
    assert store.get_active_job().state["pending_polls"] == 0


def test_a_malformed_message_destination_fails_terminally():
    """[WARP-MSG-WIDTH] Fail before the ephemeral key is minted, not after.

    Nothing checked the attested fields, and _classify treats a ValueError as
    retryable with no cap -- so a bad destination degraded into a silent
    infinite backoff, surfacing much later in the claim build.
    """
    from .test_warp_service import FakeWatcher, _seed_message_sent, _sent_msg

    store = new_store()
    engine, _ctx = build(
        store, watcher=FakeWatcher(_sent_msg(destination=bytes(20)))  # not 32
    )
    _seed_message_sent(store)

    out = engine.step()

    assert out["status"] == JobStatus.FAILED
    assert "20-byte message destination" in (out["last_error"] or "")
    assert "ephemeral_blob" not in store.get_job(out["id"]).state


def test_variable_width_message_contents_are_still_accepted():
    """Contents are heterogeneous CLVM atoms -- the amount is a short one.

    A uniform 32-byte rule here would reject every real message; this pins
    that down so the validation above cannot be tightened into a bug.
    """
    from .test_warp_service import FakeWatcher, _seed_message_sent, _sent_msg

    store = new_store()
    engine, _ctx = build(store, watcher=FakeWatcher(_sent_msg()))
    _seed_message_sent(store)

    out = engine.step()

    assert out["status"] == JobStatus.FUNDING_CLAIM
    contents = store.get_active_job().state["message_contents"]
    assert any(len(c) != 64 for c in contents), "fixture should include a short atom"


def test_the_sweep_pays_the_bot_wallet_not_the_cat_receiver(monkeypatch):
    """[WARP-SWEEP-DEST] build_sweep_bundle emits raw XCH, not a CAT.

    The destination was job.receiver_ph -- the wUSDC.b receiver, which once
    warp.chia_receiver_address is set can be an address the bot does not
    control. drivers.build_sweep_bundle's own docstring already says "the bot
    wallet"; the caller was what disagreed. Both sweep paths share this
    helper, including the unattended third-party-claim auto-sweep.
    """
    from gui.services.warp import clvm_utils as cu

    seen = {}

    def _capture(coinset, net, *, security_coin, destination_puzzle_hash,
                 ephemeral_sk, sweep_fee=0):
        seen["dest"] = destination_puzzle_hash
        return ("accepted", "ok")

    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "build_and_push_sweep", _capture)
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    store = new_store()
    engine, ctx = build(store)
    job = _seed_failed_funded(store)

    # The harness's default wallet address decodes to RECEIVER_PH, so the two
    # have to be separated for this to discriminate at all.
    wallet_ph = b"\x77" * 32
    ctx.wallet.next_address = cu.encode_puzzle_hash(wallet_ph, NET.chia_prefix)

    engine.job_action(job.id, "sweep")

    assert seen["dest"] == wallet_ph, "recovered XCH must go back to the bot wallet"
    assert seen["dest"] != RECEIVER_PH, "must not pay the wUSDC.b receiver"


def test_a_pushed_but_unspent_sweep_does_not_close_the_job(monkeypatch):
    """[WARP-SWEEP-VERIFY] Mempool acceptance is not a spent coin.

    The sweep pushes at zero fee, which is exactly what a busy mempool drops.
    Recording swept: True on acceptance closed the job as CANCELLED -- which
    offers neither Retry nor Sweep -- and left the funding coin unspent with
    no route back to it.
    """
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: ("accepted", "ok"))
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: False)
    store = new_store()
    engine, _ = build(store)
    job = _seed_failed_funded(store)

    out = engine.job_action(job.id, "sweep")

    assert out["status"] == JobStatus.FAILED, "must stay open while the coin lives"
    assert store.get_active_job() is not None
    state = store.get_active_job().state
    assert state["swept"] is False
    assert "not spent yet" in state["sweep_status"]


def test_sweep_closes_when_the_security_coin_is_provably_spent(monkeypatch):
    """Already swept, or the claim consumed it: genuinely resolved."""
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)
    store = new_store()
    engine, _ = build(store)
    job = _seed_failed_funded(store)
    store.update_job(job.id, state_patch={"security_coin_id": (b"\x5a" * 32).hex()})

    assert engine.job_action(job.id, "sweep")["status"] == JobStatus.CANCELLED


def test_sweep_keeps_the_job_open_when_the_coin_is_merely_not_found(monkeypatch):
    """Unindexed, or funded under a different claim_fee_mojos -- not proof.

    Closing here would strand the XCH: a CANCELLED row offers no Sweep, so the
    operator would have no way to try again.
    """
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: False)
    store = new_store()
    engine, _ = build(store)
    job = _seed_failed_funded(store)
    store.update_job(job.id, state_patch={"security_coin_id": (b"\x5a" * 32).hex()})

    out = engine.job_action(job.id, "sweep")

    assert out["status"] == JobStatus.FAILED
    assert store.get_active_job() is not None


def test_sweep_looks_for_the_amount_actually_funded(monkeypatch):
    """Not what current config would send -- claim_fee_mojos may have changed."""
    seen = []
    monkeypatch.setattr(
        claim_mod, "find_security_coin",
        lambda coinset, sk, expected: seen.append(expected) or FakeCoin(b"\x5a" * 32),
    )
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: ("accepted", "ok"))
    store = new_store()
    # Live config now says a *different* fee than the job was funded with.
    engine, _ = build(store, params=default_params(claim_fee_mojos=200_000_000))
    job = _seed_failed_funded(store)
    store.update_job(job.id, state_patch={"funding_amount": 4985 + 100_000_000})

    engine.job_action(job.id, "sweep")

    assert seen == [4985 + 100_000_000]


def test_sweep_transport_failure_retains_failed_and_the_slot(monkeypatch):
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))

    def boom(*a, **k):
        raise RuntimeError("coinset unreachable")

    monkeypatch.setattr(claim_mod, "build_and_push_sweep", boom)
    store = new_store()
    engine, _ = build(store)
    job = _seed_failed_funded(store)

    out = engine.job_action(job.id, "sweep")

    assert out["status"] == JobStatus.FAILED
    assert store.get_active_job() is not None  # funds still known recoverable


def test_sweep_closes_a_failed_job_that_never_reached_the_chain():
    """No ephemeral key and no bridge nonce: nothing was ever committed."""
    store = new_store()
    engine, _ = build(store)
    job = seed(store, JobStatus.FAILED, state={"failed_from": JobStatus.BRIDGING})

    out = engine.job_action(job.id, "sweep")

    assert out["status"] == JobStatus.CANCELLED
    assert store.get_active_job() is None


def test_sweep_refuses_to_close_a_job_holding_an_unclaimed_deposit():
    """The attested-terms anchors fail AFTER bridgeToChia confirms.

    Such a job has a live message and no funding coin. Closing it as "nothing
    was ever funded" would discard the only in-app record of real money.
    """
    store = new_store()
    engine, _ = build(store)
    job = seed(store, JobStatus.FAILED,
               columns={"bridge_nonce": "00" * 31 + "07",
                        "bridge_tx_hash": "0x" + "ab" * 32},
               state={"failed_from": JobStatus.MESSAGE_SENT})

    with pytest.raises(S.WarpError) as excinfo:
        engine.job_action(job.id, "sweep")

    assert "warp.green portal" in str(excinfo.value)
    assert store.get_active_job().status == JobStatus.FAILED   # still the record


def test_sweeping_a_completed_job_does_not_reopen_or_close_it(monkeypatch):
    """Only FAILED jobs need closing; COMPLETED is already closed."""
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    store = new_store()
    engine, _ = build(store)
    job = store.create_job(NET.name, status=JobStatus.AWAITING_DEPOSIT,
                           state={"hot_address": HOT})
    store.update_job(job.id, status=JobStatus.COMPLETED,
                     expected_status=JobStatus.AWAITING_DEPOSIT)

    out = engine.job_action(job.id, "sweep")

    assert out["status"] == JobStatus.COMPLETED


def test_failed_job_still_blocks_a_new_bridge():
    """The engine-side half of the dead-button bug (the widget is the other)."""
    store = new_store()
    engine, _ = build(store)
    seed(store, JobStatus.FAILED, state={"failed_from": JobStatus.CLAIMING})

    with pytest.raises(S.WarpError):
        engine.request_bridge()
    # ...and the snapshot keeps reporting it, so the widget can disable the button.
    assert engine.snapshot()["active_job"]["status"] == JobStatus.FAILED


# --------------------------------------------------------------------------- #
# F7 -- third-party-claim detection is per-nonce.
# --------------------------------------------------------------------------- #

def _seed_conflicting_claim(store, monkeypatch):
    """A CLAIMING job whose push comes back as a mempool conflict."""
    monkeypatch.setattr(claim_mod, "find_security_coin",
                        lambda *a, **k: FakeCoin(b"\x5a" * 32))
    monkeypatch.setattr(claim_mod, "claim_landed", lambda coinset, cid: False)
    monkeypatch.setattr(
        claim_mod, "sync_portal",
        lambda coinset, net, *, hint, max_hops=64: SimpleNamespace(coin_id=PORTAL),
    )
    monkeypatch.setattr(
        claim_mod, "build_and_push_claim",
        lambda coinset, net, **kw: SimpleNamespace(
            claim=SimpleNamespace(final_cat_coin_id=FINAL_CAT),
            accepted=False, status="conflict: coin already spent",
        ),
    )
    job = seed(store, JobStatus.CLAIMING,
               columns={"bridge_nonce": "00" * 31 + "07", "post_tip_mojos": 4985,
                        "receiver_ph": RECEIVER_PH.hex()},
               state={"message_destination": "00" * 32,
                      "message_contents": ["aa", "bb", "cc"],
                      "portal_coin_id": PORTAL.hex(),
                      "sigs": {str(i): bytes([0xC0 + i]).hex() for i in range(6)},
                      "security_coin_id": (b"\x5a" * 32).hex()})
    blob, _ = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob})
    return store.get_active_job()


def test_claiming_conflict_ignores_unrelated_receiver_cat_coins(monkeypatch):
    """The bot trading wUSDC.b must not look like a third party claiming.

    Against the original count-versus-baseline check this returned COMPLETED and
    swept the funding while the attested message was still unclaimed.
    """
    store = new_store()
    engine, _ = build(store)
    _seed_conflicting_claim(store, monkeypatch)
    monkeypatch.setattr(claim_mod, "message_claimed_on_chain", lambda *a, **k: False)

    def no_sweep(*a, **k):
        raise AssertionError("must not sweep a claim that is still outstanding")

    monkeypatch.setattr(claim_mod, "build_and_push_sweep", no_sweep)

    out = engine.step()

    assert out["status"] == JobStatus.CLAIMING          # re-sync, do NOT complete
    assert store.get_active_job().state["final_cat_coin_id"] == FINAL_CAT.hex()


def test_claiming_conflict_completes_when_the_message_coin_was_spent(monkeypatch):
    """A genuine third-party claim pays the same attested receiver."""
    store = new_store()
    engine, _ = build(store)
    _seed_conflicting_claim(store, monkeypatch)
    monkeypatch.setattr(claim_mod, "message_claimed_on_chain", lambda *a, **k: True)
    monkeypatch.setattr(claim_mod, "build_and_push_sweep",
                        lambda *a, **k: ("accepted", "swept"))
    # The sweep mines. Without this the coin is still live, and COMPLETED -- a
    # closed state -- must not bury it; that is the companion test below.
    monkeypatch.setattr(claim_mod, "security_coin_spent", lambda *a, **k: True)

    out = engine.step()

    assert out["status"] == JobStatus.COMPLETED
    job = store.get_job(out["id"])
    assert job.state["third_party_claim"] is True
    assert job.state["swept"] is True


def test_third_party_claim_does_not_complete_when_the_sweep_failed(monkeypatch):
    """COMPLETED is closed, so it must not bury an unswept funding coin."""
    store = new_store()
    engine, _ = build(store)
    _seed_conflicting_claim(store, monkeypatch)
    monkeypatch.setattr(claim_mod, "message_claimed_on_chain", lambda *a, **k: True)

    def unreachable(*a, **k):
        raise RuntimeError("coinset unreachable")

    monkeypatch.setattr(claim_mod, "build_and_push_sweep", unreachable)

    out = engine.step()

    assert out["status"] == JobStatus.CLAIMING          # stays open, retries
    job = store.get_active_job()
    assert job.state.get("swept") is not True
    assert "sweep failed" in job.state["sweep_status"]


def test_endless_claim_conflict_eventually_fails_into_a_sweepable_state(monkeypatch):
    """A _stay resets retry_count, so without a bound this looped forever --
    and neither Sweep nor Cancel is offered while a job sits in CLAIMING."""
    store = new_store()
    engine, _ = build(store)
    _seed_conflicting_claim(store, monkeypatch)
    monkeypatch.setattr(claim_mod, "message_claimed_on_chain", lambda *a, **k: False)

    out = None
    for _ in range(S._CLAIM_CONFLICT_MAX_ROUNDS + 2):
        out = engine.step()
        if out["status"] == JobStatus.FAILED:
            break

    assert out["status"] == JobStatus.FAILED
    assert "conflicted" in (out["last_error"] or "")
    # FAILED is the state that offers Retry and Sweep.
    assert store.get_active_job().status == JobStatus.FAILED


def test_message_coin_puzzle_hash_is_per_nonce_and_claimer_independent():
    """The fingerprint must change with the nonce and nothing else we control."""
    kw = dict(source=bytes.fromhex(NET.erc20_bridge_address[2:]),
              destination=bytes(32), contents=[b"\xaa" * 20, RECEIVER_PH, b"\x13\x79"])
    a = claim_mod.message_coin_puzzle_hash(NET, nonce=b"\x00" * 31 + b"\x07", **kw)
    b = claim_mod.message_coin_puzzle_hash(NET, nonce=b"\x00" * 31 + b"\x08", **kw)

    assert len(a) == 32
    assert a != b
    # Stable across calls: nothing about *who* claims enters the derivation.
    assert a == claim_mod.message_coin_puzzle_hash(NET, nonce=b"\x00" * 31 + b"\x07", **kw)


# --------------------------------------------------------------------------- #
# Attested source token: bytes32 word vs 20-byte address.
# --------------------------------------------------------------------------- #

def test_attested_source_token_accepts_the_padded_bytes32_word():
    """warp sends contents as bytes32; net.usdc_address is 20 bytes.

    A bare _hx() comparison is 64 chars vs 40 and can never be equal, so every
    mainnet job hit WarpTerminal at MESSAGE_SENT. Shape verified against the
    live watcher for nonce ...01d7.
    """
    from .test_warp_service import FakeWatcher, _seed_message_sent, _sent_msg

    store = new_store()
    padded = "0" * 24 + NET.usdc_address[2:].lower()
    assert len(padded) == 64
    engine, _ = build(store, watcher=FakeWatcher(_sent_msg(erc20_source=padded)))
    _seed_message_sent(store)

    out = engine.step()

    assert out["status"] == JobStatus.FUNDING_CLAIM


def test_attested_source_token_still_rejects_a_different_token():
    """The anchor must keep working -- widening must not weaken it."""
    from .test_warp_service import FakeWatcher, _seed_message_sent, _sent_msg

    store = new_store()
    wrong = "0" * 24 + "de" * 20
    engine, _ = build(store, watcher=FakeWatcher(_sent_msg(erc20_source=wrong)))
    _seed_message_sent(store)

    out = engine.step()

    assert out["status"] == JobStatus.FAILED
    assert "source token" in (out["last_error"] or "")


# --------------------------------------------------------------------------- #
# Coinset responses must be explicitly successful.
# --------------------------------------------------------------------------- #

def test_push_tx_rejects_a_response_with_no_success_field():
    """Otherwise status defaults to "SUCCESS" and a bundle that was never
    submitted is reported as accepted -- which would let a sweep be recorded
    as done without touching the chain."""
    from gui.services.warp import coinset as coinset_mod

    client = coinset_mod.CoinsetClient("https://example.invalid",
                                       poster=lambda url, body: {})
    with pytest.raises(coinset_mod.CoinsetError):
        client.push_tx({"coin_spends": []})


def test_not_found_is_none_not_an_error():
    """Coinset reports "coin not on chain yet" as success:false. That is a
    normal polling outcome, not a transport error -- treating it as one would
    drive every pending job into exponential backoff."""
    from gui.services.warp import coinset as coinset_mod

    not_found = {
        "success": False,
        "error": "Coin record ... not found",
        "structuredError": {"code": "COIN_RECORD_NOT_FOUND"},
    }
    client = coinset_mod.CoinsetClient("https://example.invalid",
                                       poster=lambda url, body: not_found)
    assert client.get_coin_record_by_name("ab" * 32) is None

    # A genuine failure still raises.
    broken = {"success": False, "error": "boom", "structuredError": {"code": "OTHER"}}
    client2 = coinset_mod.CoinsetClient("https://example.invalid",
                                        poster=lambda url, body: broken)
    with pytest.raises(coinset_mod.CoinsetError):
        client2.get_coin_record_by_name("ab" * 32)


# --------------------------------------------------------------------------- #
# F8 -- never resume a job under a different hot wallet or network.
# --------------------------------------------------------------------------- #

def test_step_refuses_a_job_bound_to_another_hot_wallet():
    store = new_store()
    engine, _ = build(store)
    seed(store, JobStatus.APPROVING,
         columns={"amount_usdc_micros": 5_000_000},
         state={"hot_address": OTHER_HOT})

    out = engine.step()

    assert out["status"] == JobStatus.APPROVING      # untouched
    assert store.get_active_job().retry_count == 0   # and unwritten
    assert "hot wallet" in (engine.snapshot()["binding_error"] or "")


def test_step_refuses_a_job_from_another_network():
    store = new_store()
    engine, _ = build(store)
    job = store.create_job("some-other-net", status=JobStatus.APPROVING,
                           state={"hot_address": HOT})

    engine.step()

    assert store.get_job(job.id).status == JobStatus.APPROVING
    assert "network" in (engine.snapshot()["binding_error"] or "")


def test_step_refuses_an_unbound_job_past_awaiting_deposit():
    """A pre-guard row cannot be proven to belong to the configured key."""
    store = new_store()
    engine, _ = build(store)
    store.create_job(NET.name, status=JobStatus.CLAIMING, state={})

    engine.step()

    assert engine.snapshot().get("binding_error")


def test_awaiting_deposit_self_heals_a_missing_binding():
    """Nothing is committed yet at AWAITING_DEPOSIT, so adopt the current key."""
    store = new_store()
    engine, ctx = build(store, params=default_params(min_micros=0))
    ctx.evm.erc20 = 5_000_000
    store.create_job(NET.name, status=JobStatus.AWAITING_DEPOSIT,
                     state={"manual": True})

    out = engine.step()

    assert out["status"] == JobStatus.DEPOSIT_SEEN
    assert store.get_active_job().state["hot_address"] == HOT


def test_retry_and_sweep_refused_on_a_mismatch_but_cancel_allowed():
    store = new_store()
    engine, _ = build(store)
    job = seed(store, JobStatus.APPROVING, state={"hot_address": OTHER_HOT})

    with pytest.raises(S.WarpError):
        engine.job_action(job.id, "retry")
    with pytest.raises(S.WarpError):
        engine.job_action(job.id, "sweep")
    # Cancel is a pure DB write and the only escape for a pre-bridge foreign job.
    assert engine.job_action(job.id, "cancel")["status"] == JobStatus.CANCELLED


def test_new_jobs_record_their_binding():
    store = new_store()
    engine, _ = build(store)

    engine.request_bridge()

    job = store.get_active_job()
    assert job.state["hot_address"] == HOT
    assert job.state["network"] == NET.name


# --------------------------------------------------------------------------- #
# F1 -- dry run signs everything and broadcasts nothing.
# --------------------------------------------------------------------------- #

def test_key_objects_never_render_their_private_key():
    """[WARP-KEY-REPR] _guarded pipes exception text into the log and the GUI.

    Both key types were plain frozen dataclasses, so the generated repr
    rendered private_key=b'\\x..' in full. One traceback holding the object
    was all it would have taken.
    """
    from gui.services.warp import keystore as ks

    secret = b"\x11" * 32
    evm = ks.evm_key_from_hex(secret.hex())
    for text in (repr(evm), str(evm), f"{evm}", f"{[evm]}"):
        assert secret.hex() not in text.lower()
        assert "\\x11" not in text
    assert evm.address in repr(evm)          # still identifiable

    bls = ks.BlsKey(private_key=secret, public_key=b"\x22" * 48)
    for text in (repr(bls), str(bls), f"{[bls]}"):
        assert secret.hex() not in text.lower()
        assert "\\x11" not in text


def test_dropping_the_engine_closes_the_job_store(tmp_path, monkeypatch):
    """[WARP-STORE-LEAK] WarpJobStore.close() had no caller anywhere.

    set_config drops the engine and _build_engine opens a fresh store, so
    every config reload leaked a connection plus its WAL/SHM handles.
    """
    from gui.services.warp.jobs import WarpJobStore

    store = WarpJobStore(str(tmp_path / "warp_jobs.db"))
    engine, _ctx = build(store)

    worker = S._WarpWorker()
    worker._engine = engine
    worker._drop_engine()

    assert worker._engine is None
    with pytest.raises(Exception):           # the connection really is closed
        store.get_active_job()


def test_an_enabled_bridge_must_state_its_cap():
    """[WARP-CAP-FAIL-OPEN] A missing cap used to mean no cap at all.

    The clamp is guarded on `if p.max_micros > 0`, and absent / blank / 0 / null
    all resolved to 0 -- so omitting the key removed the limit rather than
    setting one. Bridge now takes no amount and has no confirmation dialog, and
    the manual path skips the floor with want = have, so one click bridged the
    whole hot-wallet balance.
    """
    with pytest.raises(S.WarpError, match="max_auto_bridge_usdc is required"):
        S.warp_params_from_config({"warp": {"enabled": True}})

    with pytest.raises(S.WarpError, match="must be positive"):
        S.warp_params_from_config(
            {"warp": {"enabled": True, "max_auto_bridge_usdc": 0}}
        )

    # Uncapped is still reachable, but it has to be said out loud.
    assert S.warp_params_from_config(
        {"warp": {"enabled": True, "max_auto_bridge_usdc": "unlimited"}}
    ).max_micros == 0

    # A disabled warp section must never stop a config from loading.
    assert S.warp_params_from_config({"warp": {"enabled": False}}).max_micros == 0


def test_sweep_is_refused_engine_side_for_an_in_flight_job():
    """The GUI gates Sweep on a polled snapshot; the engine must gate it too."""
    store = new_store()
    engine, _ctx = build(store)
    job = seed(store, JobStatus.CLAIMING, columns={"receiver_ph": RECEIVER_PH.hex()})

    with pytest.raises(S.WarpError, match="still an input to the claim"):
        engine.job_action(job.id, "sweep")


def test_retry_re_signs_only_once_the_nonce_is_spent(monkeypatch):
    """[WARP-RETRY-RAW] Replaying a reverted raw fails again immediately.

    But clearing it unconditionally is worse: the stuck-tx path also lands in
    FAILED with the transaction possibly still in the mempool, and re-signing
    would take a new nonce and bridge a second time.
    """
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    job = seed(
        store, JobStatus.FAILED,
        columns={"amount_mojos": 5000, "receiver_ph": RECEIVER_PH.hex()},
        state={"failed_from": JobStatus.BRIDGING,
               "bridge_raw": "02bbbbbb", "bridge_tx_nonce": 3},
    )

    # Nonce still open -> the transaction may yet mine, so keep the raw.
    ctx.evm.mined_nonce = 3
    engine.job_action(job.id, "retry")
    assert store.get_job(job.id).state["bridge_raw"] == "02bbbbbb"

    # Nonce spent -> nothing signed can mine, so re-sign on the next tick.
    store.update_job(job.id, status=JobStatus.FAILED,
                     state_patch={"failed_from": JobStatus.BRIDGING})
    ctx.evm.mined_nonce = 9
    engine.job_action(job.id, "retry")
    assert store.get_job(job.id).state.get("bridge_raw") is None


def _seed_broadcast_bridge(store):
    seed(store, JobStatus.BRIDGING,
         columns={"amount_mojos": 5000, "receiver_ph": RECEIVER_PH.hex()})


def test_an_unmined_bridge_is_re_signed_at_a_higher_fee(monkeypatch):
    """[WARP-STUCK-TX] bump_fees finally has a caller.

    An unmined bridge used to sit in a bare _stay forever. _apply_stay resets
    retry_count and clears last_error on every pend, so the loop was
    invisible; _BRIDGE_MAX_ATTEMPTS only counts reverted receipts, never an
    absent one. BRIDGING is not cancellable and Sweep closes only FAILED, so
    the job held the single active row permanently and every later bridge
    raised ActiveJobExists -- recoverable only by editing warp_jobs.db.
    """
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    _seed_broadcast_bridge(store)

    engine.step()                                   # phase 1: sign
    original = store.get_active_job().state["bridge_fees"]
    ctx.evm.receipt = None                          # never mines

    for _ in range(S._STUCK_TX_POLLS):   # the last one triggers the bump
        engine.step()

    job = store.get_active_job()
    assert job.status == JobStatus.BRIDGING
    assert job.state["bridge_fee_bumps"] == 1, "should have escalated exactly once"
    assert job.state["bridge_unmined_polls"] == 0, "poll counter resets after a bump"
    # The replacement is signed against the ORIGINAL fees, so consecutive
    # attempts stay a full ratio apart and clear the node's replacement floor.
    bumped = ctx.evm.prepared[-1]
    assert bumped[0] == "bridge"
    assert store.get_active_job().state["bridge_fees"] == original


def test_a_bridge_nonce_consumed_elsewhere_fails_instead_of_pending(monkeypatch):
    """If something else took the nonce, nothing we signed can ever mine."""
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    _seed_broadcast_bridge(store)

    engine.step()                                   # signs at nonce 3
    ctx.evm.receipt = None
    ctx.evm.mined_nonce = 9                         # account moved past it

    out = engine.step()

    assert out["status"] == JobStatus.FAILED
    assert "consumed by another transaction" in (store.get_active_job().last_error or "")


def test_a_replacement_still_polls_the_original_hash(monkeypatch):
    """Only one signing of a nonce can mine, and the node picks which."""
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    _seed_broadcast_bridge(store)

    engine.step()
    first_hash = store.get_active_job().bridge_tx_hash
    ctx.evm.receipt = None
    for _ in range(S._STUCK_TX_POLLS):   # the last one triggers the bump
        engine.step()

    assert first_hash in store.get_active_job().state["bridge_prior_hashes"]


def test_a_receipt_without_a_status_is_not_treated_as_success(monkeypatch):
    """[WARP-RECEIPT-AMBIG] "not reverted" is not "succeeded".

    Both handlers branched on receipt_reverted alone, so a receipt whose
    status is absent or unparseable took the success path -- advancing to
    BRIDGING on an allowance that may never have taken effect.
    """
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    seed(store, JobStatus.APPROVING,
         columns={"amount_usdc_micros": 5_000_000, "amount_mojos": 5000})

    engine.step()                                   # phase 1: sign
    ctx.evm.receipt = {"blockNumber": "0x10"}       # mined, but no status field

    out = engine.step()

    assert out["status"] == JobStatus.APPROVING, "must not advance on an unknown status"
    assert store.get_active_job().state["approve_status_polls"] == 1


def test_an_ambiguous_receipt_eventually_fails_instead_of_pending_forever(monkeypatch):
    """The re-poll is bounded: FAILED is the status with operator affordances."""
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    seed(store, JobStatus.APPROVING,
         columns={"amount_usdc_micros": 5_000_000, "amount_mojos": 5000})

    engine.step()
    ctx.evm.receipt = {"blockNumber": "0x10"}
    out = None
    for _ in range(S._AMBIGUOUS_RECEIPT_POLLS + 2):
        out = engine.step()
        if out["status"] == JobStatus.FAILED:
            break

    assert out["status"] == JobStatus.FAILED


def test_estimate_gas_refuses_to_broadcast_a_reverting_call():
    """[WARP-ESTIMATE-REVERT] A revert during estimation must not fall back.

    Falling back to a fixed gas limit and broadcasting anyway spends the gas
    to be told on chain what the node already said. A transport error is
    different -- it says nothing about the transaction, and the fallback is
    exactly right there.
    """
    from gui.services.warp.evm import EvmError, EvmRpcError

    class _Client(evm_mod.EvmClient):
        def __init__(self, exc):
            self._exc = exc

        def _call_quantity(self, method, params):
            raise self._exc

    reverting = _Client(EvmRpcError("execution reverted: ERC20: bad allowance", code=3))
    with pytest.raises(EvmError):
        reverting.estimate_gas(
            from_address="0x" + "ab" * 20, to="0x" + "cd" * 20,
            value=0, data=b"", default=250_000,
        )

    # A node that is merely unreachable still gets the fallback.
    offline = _Client(EvmError("eth_estimateGas request failed: connection reset"))
    assert offline.estimate_gas(
        from_address="0x" + "ab" * 20, to="0x" + "cd" * 20,
        value=0, data=b"", default=250_000,
    ) == 250_000


def test_editing_claim_fee_does_not_orphan_a_funded_job(monkeypatch):
    """[WARP-FEE-FREEZE] Lookups use the funded amount, not today's config.

    find_security_coin matches the on-chain amount exactly. The claim-path
    handlers recomputed it as post_tip + claim_fee_mojos from live params, so
    editing that key and reloading made every scan look for an amount no coin
    has. The job then pended forever -- _apply_stay clears last_error and
    resets retry_count on each pend, so it never reached FAILED, and a
    non-FAILED job offers neither Retry nor Sweep.
    """
    store = new_store()
    job = seed(
        store, JobStatus.CLAIM_FUNDED,
        columns={"receiver_ph": RECEIVER_PH.hex(), "post_tip_mojos": 4985},
    )
    blob, _sk = make_ephemeral_blob(job.id)
    funded = 4985 + 100_000_000            # what was actually sent
    store.update_job(
        job.id,
        state_patch={"ephemeral_blob": blob, "funding_amount": funded},
    )

    # Operator edits claim_fee_mojos and the config reloads.
    engine, ctx = build(store, params=default_params(claim_fee_mojos=250_000_000))

    looked_for = []
    monkeypatch.setattr(
        claim_mod, "find_security_coin",
        lambda coinset, sk, amount: looked_for.append(amount),
    )

    engine.step()

    assert looked_for == [funded], (
        f"scanned for {looked_for} but the coin on chain holds {funded}"
    )


def test_flipping_dry_run_cannot_close_a_live_broadcast_job(monkeypatch):
    """[WARP-DRYRUN-FREEZE] A live job stays live when the config flips.

    The rehearsal stops used to read self._params.dry_run. A job broadcast
    live and sitting in BRIDGING would, the moment warp.dry_run went true,
    be closed as DRY_RUN_OK with bridge_tx_hash nulled -- freeing the slot
    and leaving an audit trail saying "no funds moved" while real USDC was
    in flight, recoverable only by BaseScan forensics. dry_run defaults to
    true, so restarting the GUI was enough to trigger it.
    """
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    # Engine now configured for rehearsal; the job was frozen live.
    engine, ctx = build(store, params=default_params(dry_run=True))
    seed(
        store, JobStatus.BRIDGING,
        columns={"amount_mojos": 5000, "receiver_ph": RECEIVER_PH.hex()},
        state={"dry_run": False},
    )

    engine.step()                                   # phase 1: sign
    job = store.get_active_job()
    assert job.status == JobStatus.BRIDGING
    assert job.bridge_tx_hash == "0x" + "22" * 32

    ctx.evm.receipt = message_sent_receipt("00" * 31 + "07")
    out = engine.step()                             # phase 2: broadcast

    assert out["status"] == JobStatus.BRIDGE_CONFIRMED, "a live job must not go DRY_RUN_OK"
    assert ctx.evm.sent_raw == [b"\x02\xbb\xbb\xbb"], "the live job must still broadcast"
    assert store.get_active_job().bridge_tx_hash == "0x" + "22" * 32


def test_a_job_predating_the_dry_run_freeze_is_refused_not_guessed(monkeypatch):
    """A row with no frozen dry_run cannot be resumed in APPROVING/BRIDGING.

    Neither default is safe: assume rehearsal and a live job is closed with
    its tx hash discarded; assume live and a rehearsal broadcasts real funds.
    """
    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    # Bypass seed(), which freezes dry_run -- this is a pre-freeze row, whose
    # state carries the old binding and nothing else.
    store.create_job(
        NET.name,
        status=JobStatus.BRIDGING,
        columns={"amount_mojos": 5000, "receiver_ph": RECEIVER_PH.hex()},
        state={"network": NET.name, "hot_address": "ab" * 20},
    )
    assert "dry_run" not in store.get_active_job().state

    engine.step()

    # A binding mismatch is read-only: it refuses before the handler runs, so
    # phase 1 never signs. Asserting on sent_raw alone would be vacuous here --
    # a single BRIDGING step never broadcasts even on the happy path.
    job = store.get_active_job()
    assert "bridge_raw" not in job.state, "a job that cannot be classified must not sign"
    assert job.bridge_tx_hash is None
    assert ctx.evm.sent_raw == []
    assert job.status == JobStatus.BRIDGING
    assert engine._binding_error and "predates the dry_run freeze" in engine._binding_error


def test_dry_run_signs_both_txs_and_never_broadcasts(monkeypatch):
    monkeypatch.setattr(
        evm_mod, "sign_tx",
        lambda unsigned, key: SimpleNamespace(
            raw=b"\xde\xad\xbe\xef", tx_hash="0x" + "11" * 32, nonce=1
        ),
    )
    store = new_store()
    engine, ctx = build(store, params=default_params(dry_run=True, min_micros=0))

    def never(*a, **k):
        raise AssertionError("a dry run must not broadcast")

    ctx.evm.send_raw_transaction = never
    ctx.evm.erc20 = 5_000_000
    ctx.evm.allowance = 0
    seed(store, JobStatus.AWAITING_DEPOSIT, state={"manual": True})

    seen = []
    out = None
    for _ in range(8):
        out = engine.step()
        seen.append(out["status"])
        if out["status"] == JobStatus.DRY_RUN_OK:
            break

    assert seen[-1] == JobStatus.DRY_RUN_OK
    assert JobStatus.APPROVING in seen and JobStatus.BRIDGING in seen
    # DRY_RUN_OK is closed, so the slot is free and a real run can follow.
    assert store.get_active_job() is None
    # No BaseScan link for a transaction that was never sent.
    assert store.get_job(out["id"]).bridge_tx_hash is None


def test_dry_run_never_auto_starts_jobs():
    """A rehearsal cannot spend the balance and DRY_RUN_OK frees the slot, so
    auto-bridging one would loop forever with no fixed point."""
    store = new_store()
    engine, ctx = build(store, params=default_params(
        enabled=True, auto_bridge=True, dry_run=True, min_micros=1_000_000))
    ctx.evm.erc20 = 500_000_000

    assert engine.maybe_start_auto_job() is None
    assert store.get_active_job() is None


def test_auto_bridge_still_starts_when_live():
    store = new_store()
    engine, ctx = build(store, params=default_params(
        enabled=True, auto_bridge=True, dry_run=False, min_micros=1_000_000))
    ctx.evm.erc20 = 500_000_000

    assert engine.maybe_start_auto_job() is not None


def test_testnet_config_key_is_rejected():
    with pytest.raises(S.WarpError):
        S.warp_params_from_config({"warp": {"enabled": True, "testnet": True}})
    # A leftover falsy value stays harmless.
    assert S.warp_params_from_config({"warp": {"testnet": False}}).enabled is False


def test_dry_run_defaults_on():
    cap = {"max_auto_bridge_usdc": 100}      # required once enabled
    assert S.warp_params_from_config(
        {"warp": {"enabled": True, **cap}}
    ).dry_run is True
    assert S.warp_params_from_config(
        {"warp": {"enabled": True, "dry_run": False, **cap}}
    ).dry_run is False


def test_only_mainnet_remains():
    assert not hasattr(C, "TESTNET")
    assert not hasattr(C, "net_for")
    assert C.MAINNET.expected_asset_id


# --------------------------------------------------------------------------- #
# F6 -- the effective receiver is resolved, cached, and published.
# --------------------------------------------------------------------------- #

def test_snapshot_publishes_the_wallet_derived_receiver():
    store = new_store()
    engine, ctx = build(store, params=default_params(chia_receiver_address=""))
    ctx.wallet.next_address = ADDR

    engine.refresh_hot_wallet()
    snap = engine.snapshot()

    assert snap["receiver_address"] == ADDR
    assert snap["receiver_source"] == "wallet"


def test_receiver_is_resolved_once_and_never_mints_a_fresh_address():
    """new_address=True burned a derivation index on every single call."""
    store = new_store()
    engine, ctx = build(store, params=default_params(chia_receiver_address=""))
    calls = []
    original = ctx.wallet.get_next_address

    def counting(wallet_id, new_address=True):
        calls.append(new_address)
        return original(wallet_id, new_address=new_address)

    ctx.wallet.get_next_address = counting

    engine.refresh_hot_wallet()
    engine.snapshot()
    engine.refresh_hot_wallet()

    assert len(calls) == 1
    assert calls[0] is False


def test_configured_receiver_wins_and_is_labelled():
    store = new_store()
    engine, _ = build(store, params=default_params(chia_receiver_address=ADDR))

    engine.refresh_hot_wallet()

    assert engine.snapshot()["receiver_source"] == "config"


def test_effective_receiver_never_raises_when_the_wallet_is_down():
    store = new_store()
    engine, ctx = build(store, params=default_params(chia_receiver_address=""))

    def boom(*a, **k):
        raise RuntimeError("wallet daemon not running")

    ctx.wallet.log_in = boom

    engine.refresh_hot_wallet()  # contracted never to raise

    assert engine.snapshot()["receiver_address"] == ""
    assert "unavailable" in engine.snapshot()["receiver_source"]


# --------------------------------------------------------------------------- #
# F10 -- the wrapped-asset anchor runs offline, before anything is built.
# --------------------------------------------------------------------------- #

def test_derived_asset_id_matches_the_mainnet_anchor():
    """Pure CLVM currying, no RPC -- which is what lets it run at startup."""
    assert drivers_mod.derive_wrapped_asset_id(NET).hex() == NET.expected_asset_id


def test_anchor_accepts_a_matching_configured_id():
    derived = drivers_mod.verify_wrapped_asset_anchor(NET, NET.expected_asset_id)
    assert derived.hex() == NET.expected_asset_id
    # 0x-prefixed and mixed case are both accepted.
    drivers_mod.verify_wrapped_asset_anchor(NET, "0x" + NET.expected_asset_id.upper())


def test_anchor_rejects_a_typod_configured_id():
    with pytest.raises(drivers_mod.WarpDriverError):
        drivers_mod.verify_wrapped_asset_anchor(NET, "ff" * 32)


def test_anchor_rejects_an_empty_deployment_constant():
    import dataclasses

    broken = dataclasses.replace(NET, expected_asset_id="")
    with pytest.raises(drivers_mod.WarpDriverError):
        drivers_mod.verify_wrapped_asset_anchor(broken)


def test_build_engine_checks_the_anchor_before_constructing_clients(monkeypatch):
    """A mismatch must block *before* an EvmClient or job store exists."""
    from gui.services.warp import coinset as coinset_mod

    def must_not_run(*a, **k):
        raise AssertionError("no client may be constructed before the anchor runs")

    monkeypatch.setattr(evm_mod, "EvmClient", must_not_run)
    monkeypatch.setattr(coinset_mod, "CoinsetClient", must_not_run)

    worker = S._WarpWorker()
    worker.set_config({
        "warp": {
            "enabled": True,
            "max_auto_bridge_usdc": 100,             # required once enabled
            "expected_asset_id": "ff" * 32,          # deliberately wrong
            "evm_private_key_dpapi": "irrelevant",
        }
    })

    assert worker._ensure_engine() is None
    assert "anchor" in (worker._engine_error or "")
