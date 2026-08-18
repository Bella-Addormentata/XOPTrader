"""XCH fee gates: G10's double-payment budget and the sending-tick re-checks.

The 2026-08 fee study traced every warp XCH draw to the wallet that pays it
and found two gaps, both harmless at default config and wrong the day the
knobs move:

1. ``unwrap_chia_fee_mojos`` is paid TWICE per unwrap (cat_spend + toll
   funding send) but G10 budgeted it once -- a wallet holding exactly
   toll + fee passed the gate and stalled mid-burn.
2. The XCH checks ran only at UNWRAP_CHECKS; the trading engine can lock
   XCH into offer collateral between the gate and the spend, and the wrap
   claim-funding send had no balance preflight at all.  A shortfall
   surfaced as a generic wallet-daemon error, not a named pend.

These tests pin the corrected budget and each sending-tick re-check.
"""

from __future__ import annotations

import pytest

from gui.services.warp import claim as claim_mod
from gui.services.warp import service as S
from gui.services.warp.jobs import JobStatus

from .test_warp_service import build, make_ephemeral_blob, new_store, seed
from .test_warp_unwrap_service import (  # noqa: F401
    NET,
    build_unwrap,
    params_with_cap,
    request,
)

_TOLL = NET.chia_toll_mojos
_FEE = 50_000_000  # 0.00005 XCH -- a plausible congestion-priority fee


# --------------------------------------------------------------------------- #
# G10: the gate budgets BOTH fee payments.
# --------------------------------------------------------------------------- #

def test_g10_budgets_the_fee_for_both_spends():
    """toll + 1 fee (the old gate's ask) must PEND; toll + 2 fees passes.

    The fee is paid on the cat_spend AND on the toll funding send; a wallet
    that can only cover one of them must not start a burn it cannot finish.
    """
    store = new_store()
    engine, ctx = build_unwrap(
        store, params=params_with_cap(unwrap_chia_fee_mojos=_FEE)
    )
    one_fee = _TOLL + _FEE
    ctx.wallet.balances[1] = {
        "confirmed_wallet_balance": one_fee, "spendable_balance": one_fee,
    }
    request(engine)
    out = engine.step()
    assert out["status"] == JobStatus.UNWRAP_CHECKS, "must pend, not proceed"
    assert out["retry_count"] == 0, "a healthy pend, not an error"
    assert ctx.wallet.cat_spends == []

    # The full budget frees (offers closed) -> the same job proceeds.
    two_fees = _TOLL + 2 * _FEE
    ctx.wallet.balances[1] = {
        "confirmed_wallet_balance": two_fees, "spendable_balance": two_fees,
    }
    assert engine.step()["status"] == JobStatus.BURN_SENT


def test_g10_zero_fee_needs_only_the_toll():
    """The default zero-fee path is unchanged: exactly the toll suffices."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    ctx.wallet.balances[1] = {
        "confirmed_wallet_balance": _TOLL, "spendable_balance": _TOLL,
    }
    request(engine)
    assert engine.step()["status"] == JobStatus.BURN_SENT


# --------------------------------------------------------------------------- #
# The helper: the message must NAME the shortfall.
# --------------------------------------------------------------------------- #

def test_require_spendable_xch_names_context_and_amounts():
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())
    ctx.wallet.balances[1]["spendable_balance"] = 5

    with pytest.raises(S.WarpPending) as ei:
        engine._require_spendable_xch(100, context="toll funding")
    msg = str(ei.value)
    assert "toll funding" in msg
    assert "5" in msg and "100" in msg
    assert "collateral" in msg, "the operator is told WHY it is short"


def test_require_spendable_xch_zero_needed_is_a_free_noop():
    """needed <= 0 must not even hit the wallet RPC (default-fee paths)."""
    store = new_store()
    engine, ctx = build_unwrap(store, params=params_with_cap())

    calls = {"n": 0}
    original = ctx.wallet.get_wallet_balance

    def counting(wallet_id):
        calls["n"] += 1
        return original(wallet_id)

    ctx.wallet.get_wallet_balance = counting
    engine._require_spendable_xch(0, context="x")
    engine._require_spendable_xch(-1, context="x")
    assert calls["n"] == 0


# --------------------------------------------------------------------------- #
# Wiring: each sending tick re-checks before it sends.
# --------------------------------------------------------------------------- #

def test_funding_claim_pends_named_when_collateral_locks(monkeypatch):
    """The wrap claim-funding send previously had NO balance preflight."""
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    monkeypatch.setattr(
        claim_mod, "security_coin_puzzle_hash", lambda sk: bytes(32)
    )
    funding_fee = 25_000_000
    store = new_store()
    engine, ctx = build(
        store, params=params_with_cap(chia_funding_fee_mojos=funding_fee)
    )
    job = seed(store, JobStatus.FUNDING_CLAIM, columns={"post_tip_mojos": 4985})
    blob, _ = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob})

    # One mojo under post_tip + claim fee + funding fee: a weakened check
    # that omitted the funding fee would PASS this balance and send.
    expected = 4985 + 100_000_000
    ctx.wallet.balances[1]["spendable_balance"] = expected + funding_fee - 1

    out = engine.step()
    assert out["status"] == JobStatus.FUNDING_CLAIM, "pends in place"
    assert out["retry_count"] == 0
    assert ctx.wallet.sent == [], "the send must not be attempted short"

    # Collateral frees -> the same job funds on the next tick.
    ctx.wallet.balances[1]["spendable_balance"] = 10 ** 12
    engine.step()
    assert len(ctx.wallet.sent) == 1


def test_burning_pends_named_when_collateral_locks(monkeypatch):
    """The toll funding send re-checks toll + fee on the sending tick."""
    from types import SimpleNamespace

    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: None)
    store = new_store()
    engine, ctx = build_unwrap(
        store, params=params_with_cap(unwrap_chia_fee_mojos=_FEE)
    )
    job = seed(
        store, JobStatus.BURNING,
        columns={"amount_mojos": 5000},
        state={"direction": "out", "receiver_evm": "ab" * 20,
               "burn_cat_ph": "bb" * 32, "cat_wallet_id": 3},
    )
    blob, _sk = make_ephemeral_blob(job.id)
    store.update_job(job.id, state_patch={"ephemeral_blob": blob})
    ctx.coinset.get_coin_record_by_name = lambda name: None
    ctx.coinset.by_ph = [
        SimpleNamespace(
            coin=SimpleNamespace(parent_coin_info="77" * 32,
                                 puzzle_hash="bb" * 32, amount=5000),
            spent=False, confirmed_block_index=100, spent_block_index=0,
        )
    ]

    # G10 passed long ago; the engine has since locked the XCH.  One mojo
    # under toll + fee: a weakened check that counted only the toll would
    # PASS this balance and send -- this pins the fee term specifically.
    ctx.wallet.balances[1]["spendable_balance"] = _TOLL + _FEE - 1

    out = engine.step()
    assert out["status"] == JobStatus.BURNING, "pends in place"
    assert out["retry_count"] == 0
    assert ctx.wallet.sent == [], "the toll must not be funded short"

    # Frees -> funds.
    ctx.wallet.balances[1]["spendable_balance"] = 10 ** 12
    engine.step()
    assert len(ctx.wallet.sent) == 1
    assert ctx.wallet.sent[0][0] == _TOLL
    assert ctx.wallet.sent[0][2] == _FEE, "the toll send carries the fee"


def test_burn_sent_pends_named_when_fee_collateral_locks():
    """The cat_spend fee re-check fires on the tick that sends the burn.

    G10 budgeted the fee at UNWRAP_CHECKS; by the BURN_SENT tick the engine
    may have locked that XCH into offers.  The burn must not be attempted
    with an unpayable fee.
    """
    store = new_store()
    engine, ctx = build_unwrap(
        store, params=params_with_cap(unwrap_chia_fee_mojos=_FEE)
    )
    request(engine)
    out = engine.step()
    assert out["status"] == JobStatus.BURN_SENT
    assert ctx.wallet.cat_spends == [], "burn not yet sent on the gate tick"

    # Engine locks the XCH between ticks.
    ctx.wallet.balances[1]["spendable_balance"] = _FEE - 1
    out = engine.step()
    assert out["status"] == JobStatus.BURN_SENT, "pends in place"
    assert out["retry_count"] == 0
    assert ctx.wallet.cat_spends == []

    # Frees -> burns.
    ctx.wallet.balances[1]["spendable_balance"] = 10 ** 12
    engine.step()
    assert len(ctx.wallet.cat_spends) == 1



if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
