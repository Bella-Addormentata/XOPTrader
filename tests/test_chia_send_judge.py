"""[WARP-LOST-SEND 2026-10-25] The verdict on a recorded Chia send, offline.

gui/services/warp/chia_send.py imports no chia library, so these run in
every environment.  The evidence shapes are job 4 of 2026-10-24: a claim's
funding send whose coin went to an engine offer's spend, and which the
engine's stuck-transaction cleanup then deleted from the wallet.
"""

from __future__ import annotations

from types import SimpleNamespace

import pytest

from gui.services.warp import chia_send as cs

PARENT = "c4a91ced4257254eb8a1e7c166de014eb3daaedd0f8a8c0ef6f41cfc09847d3c"
OUT_PH = "fb0d169bf08350a0212e14a70accd2c3cb829d3e2f3037f42435336535cea173"
OUT_AMOUNT = 100_004_985


def _coin(parent, ph, amount):
    return SimpleNamespace(coin=SimpleNamespace(
        parent_coin_info=parent, puzzle_hash=ph, amount=amount))


def _gather(*, wallet=None, wallet_raises=False, mempool=None, spent=None,
            outputs=(), removals=(), unique=True):
    def wallet_record():
        if wallet_raises:
            raise RuntimeError("rpc down")
        return wallet

    return cs.gather(
        wallet_record=wallet_record,
        mempool_item=lambda: mempool,
        coin_spent=lambda cid: (spent or {}).get(cid),
        outputs_at=lambda: list(outputs),
        known_removals=removals, out_amount=OUT_AMOUNT, unique_output=unique,
    )


def test_coin_ids_match_chias_encoding():
    """The id of a 499.005 XCH coin, against the repo's own coin_name."""
    cu = pytest.importorskip("gui.services.warp.clvm_utils")
    parent, ph, amount = "11" * 32, "22" * 32, 499_005_000_000
    assert cs.coin_id(parent, ph, amount) == cu.coin_name(
        bytes.fromhex(parent), bytes.fromhex(ph), amount).hex()
    # The sign bit matters: 128 encodes with a leading zero byte.
    assert cs._int_to_bytes(128) == b"\x00\x80"
    assert cs._int_to_bytes(0) == b""
    assert cs._int_to_bytes(1) == b"\x01"


def test_removal_ids_read_the_coins_a_record_spent():
    rec = {"removals": [
        {"parent_coin_info": "0x" + "11" * 32, "puzzle_hash": "0x" + "22" * 32,
         "amount": 499_005_000_000},
        {"garbage": True},
        {"parent_coin_info": "zz", "puzzle_hash": "0x" + "22" * 32, "amount": 1},
    ]}
    ids = cs.removal_ids(rec)
    assert ids == (cs.coin_id("11" * 32, "22" * 32, 499_005_000_000),)
    assert cs.removal_ids(None) == ()
    assert cs.removal_ids({"removals": "nope"}) == ()


def test_job_4_is_dead():
    """Gone from the wallet, its input spent by another spend, no output."""
    rid = "c" * 64
    ev = _gather(wallet=None, mempool=None, spent={rid: True}, removals=[rid])
    assert ev.removal_spent is True and ev.wallet_known is False
    verdict, why = cs.judge(ev)
    assert verdict is cs.Verdict.DEAD
    assert "another transaction" in why


def test_an_input_spent_by_our_own_send_means_landed():
    rid = "c" * 64
    ev = _gather(wallet=None, mempool=None, spent={rid: True}, removals=[rid],
                 outputs=[_coin(rid, OUT_PH, OUT_AMOUNT)])
    assert cs.judge(ev)[0] is cs.Verdict.LANDED


def test_a_shared_output_puzzle_needs_our_parent():
    """burn_cat_ph is shared across unwraps: a coin of the right amount from
    someone else's spend is not our output."""
    rid = "c" * 64
    other = _coin("d" * 64, OUT_PH, OUT_AMOUNT)
    ev = _gather(wallet=None, mempool=None, spent={rid: True}, removals=[rid],
                 outputs=[other], unique=False)
    assert cs.judge(ev)[0] is cs.Verdict.DEAD
    ev2 = _gather(wallet=None, mempool=None, spent={rid: True}, removals=[rid],
                  outputs=[other], unique=True)
    assert cs.judge(ev2)[0] is cs.Verdict.LANDED


def test_the_wallet_holding_it_means_alive():
    rid = "c" * 64
    ev = _gather(wallet={"confirmed": False, "removals": []},
                 spent={rid: False}, removals=[rid])
    assert cs.judge(ev)[0] is cs.Verdict.ALIVE
    ev = _gather(wallet={"confirmed": True}, spent={rid: False}, removals=[rid])
    assert cs.judge(ev)[0] is cs.Verdict.LANDED


def test_the_mempool_holding_it_means_alive_and_teaches_the_removals():
    item = {"removals": [
        {"parent_coin_info": "0x" + "11" * 32, "puzzle_hash": "0x" + "22" * 32,
         "amount": 5}]}
    ev = _gather(wallet=None, mempool=item, spent={})
    assert cs.judge(ev)[0] is cs.Verdict.ALIVE
    assert ev.removals == (cs.coin_id("11" * 32, "22" * 32, 5),)


def test_nowhere_with_unspent_inputs_is_an_orphan():
    rid = "c" * 64
    ev = _gather(wallet=None, mempool=None, spent={rid: False}, removals=[rid])
    verdict, why = cs.judge(ev)
    assert verdict is cs.Verdict.ORPHAN and "unspent" in why
    # ...and so is one whose inputs nobody recorded.
    ev = _gather(wallet=None, mempool=None, spent={})
    verdict, why = cs.judge(ev)
    assert verdict is cs.Verdict.ORPHAN and "not known" in why


def test_an_unanswered_probe_is_unknown_not_dead():
    rid = "c" * 64
    # The wallet did not answer: nothing can be concluded from "no record".
    ev = _gather(wallet_raises=True, mempool=None, spent={rid: False},
                 removals=[rid])
    assert ev.wallet_known is None
    assert cs.judge(ev)[0] is cs.Verdict.UNKNOWN
    # An input the chain does not index yet leaves removal_spent unknown.
    ev = _gather(wallet=None, mempool=None, spent={}, removals=[rid])
    assert ev.removal_spent is None
    assert cs.judge(ev)[0] is cs.Verdict.UNKNOWN


def test_a_spent_input_outranks_the_wallets_optimism():
    """The wallet keeps retrying a send whose coin is gone (DOUBLE_SPEND
    forever); the chain's verdict wins."""
    rid = "c" * 64
    ev = _gather(wallet={"confirmed": False}, spent={rid: True}, removals=[rid])
    assert cs.judge(ev)[0] is cs.Verdict.DEAD


def test_a_failing_output_probe_does_not_hide_a_dead_send():
    rid = "c" * 64

    def boom():
        raise RuntimeError("coinset down")

    ev = cs.gather(wallet_record=lambda: None, mempool_item=lambda: None,
                   coin_spent=lambda cid: True, outputs_at=boom,
                   known_removals=[rid], out_amount=OUT_AMOUNT, unique_output=True)
    assert ev.output_landed is None
    assert cs.judge(ev)[0] is cs.Verdict.DEAD
