"""Multi-asset inbound plumbing: the asset travels with the job.

PR B1 threads a :class:`WarpAsset` descriptor through the Base->Chia path so
every amount, contract call and fail-closed anchor refers to the token the
job was created for. USDC behaviour is unchanged (the rest of the suite is
that proof); these tests pin the machinery the generalisation adds.

The load-bearing hazard is the mojo factor: USDC is 6-decimal against
3-decimal CATs (factor 1000) while milliETH is itself 3-decimal (factor 1),
so reusing one asset's scale for another mis-sizes a bridge by 1000x. That
is why the descriptor -- address AND decimals together -- is what gets
passed around, never a bare address.
"""

from __future__ import annotations

import pytest

pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")

from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import evm  # noqa: E402
from gui.services.warp import service as S  # noqa: E402

from .test_warp_service import (  # noqa: E402
    RECEIVER_PH, FakeWatcher, _sent_msg, build, default_params, fake_sign_tx,
    new_store, seed,
)

NET = C.MAINNET
USDC = NET.asset("USDC")
MILLI = NET.asset("milliETH")


# --------------------------------------------------------------------------- #
# The 1000x hazard: scale is welded to the asset.
# --------------------------------------------------------------------------- #

def test_the_mojo_factor_is_per_asset():
    assert evm._mojo_factor(NET, USDC) == 1000, "6-dec token vs 3-dec CAT"
    assert evm._mojo_factor(NET, MILLI) == 1, "3-dec token vs 3-dec CAT"
    assert evm._mojo_factor(NET, None) == 1000, "default stays USDC"

    # 1 wmilliETH.b mojo is one milliETH base unit, not a thousand.
    assert evm.mojo_to_base_units(1000, NET, MILLI) == 1000
    assert evm.mojo_to_base_units(1000, NET, USDC) == 1_000_000
    assert evm.bridgeable_mojo(1_500, NET, MILLI) == 1_500
    assert evm.bridgeable_mojo(1_500_999, NET, USDC) == 1_500


def test_amount_round_trips_within_one_mojo_for_both_assets():
    for spec in (USDC, MILLI):
        for base in (1, 999, 1_000, 1_234_567):
            mojo = evm.base_units_to_mojo(base, NET, spec)
            back = evm.mojo_to_base_units(mojo, NET, spec)
            assert back <= base, spec.symbol
            assert base - back < evm._mojo_factor(NET, spec), spec.symbol


# --------------------------------------------------------------------------- #
# Contract calls target the asset's own token.
# --------------------------------------------------------------------------- #

def _fees():
    from types import SimpleNamespace
    return SimpleNamespace(max_fee_per_gas=10 ** 9, max_priority_fee_per_gas=10 ** 6)


def test_approve_and_bridge_encode_the_assets_token():
    approve = evm.build_approve_tx(NET, amount_base_units=1000, nonce=0,
                                   fees=_fees(), gas=80_000, asset=MILLI)
    assert approve.to.hex() == MILLI.erc20_address[2:].lower()
    usdc_approve = evm.build_approve_tx(NET, amount_base_units=1000, nonce=0,
                                        fees=_fees(), gas=80_000)
    assert usdc_approve.to.hex() == NET.usdc_address[2:].lower(), \
        "default is still USDC"

    bridge = evm.build_bridge_tx(NET, receiver_ph=bytes.fromhex("cd" * 32),
                                 mojo_amount=1000, toll_wei=1, nonce=0,
                                 fees=_fees(), gas=200_000, asset=MILLI)
    assert MILLI.erc20_address[2:].lower() in bridge.data.hex()
    assert NET.usdc_address[2:].lower() not in bridge.data.hex()


# --------------------------------------------------------------------------- #
# The job carries its asset, and an unknown one is terminal.
# --------------------------------------------------------------------------- #

def _engine(**kw):
    """(store, engine, ctx) from the service suite's own harness."""
    store = new_store()
    engine, ctx = build(store, params=default_params(**kw))
    return store, engine, ctx


def test_a_job_without_an_asset_reads_as_usdc():
    """Rows written before asset stamping existed can only have been USDC --
    it was the sole bridgeable token."""
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT, state={})
    assert eng._job_asset(job).symbol == "USDC"


def test_a_stamped_job_resolves_to_its_own_asset():
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT, state={"asset": "milliETH"})
    spec = eng._job_asset(job)
    assert spec.symbol == "milliETH"
    assert spec.erc20_address == NET.milli_eth_address
    assert spec.erc20_decimals == 3


def test_an_unknown_asset_is_terminal_not_a_silent_fallback():
    """Guessing here would size amounts and derive the wrapped TAIL against
    the wrong token."""
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT, state={"asset": "DOGE"})
    with pytest.raises(S.WarpTerminal, match="DOGE"):
        eng._job_asset(job)


def test_new_jobs_stamp_their_asset():
    store, eng, ctx = _engine()
    out = eng.request_bridge()
    row = store.get_job(out["id"])
    assert row.state.get("asset") == "USDC"


# --------------------------------------------------------------------------- #
# The attestation anchor follows the job, not a global constant.
# --------------------------------------------------------------------------- #

def test_the_attested_source_anchor_compares_against_the_jobs_asset():
    """A milliETH job must REJECT a USDC-sourced attestation, and accept a
    milliETH-sourced one. Before this change every job compared against
    net.usdc_address, so a milliETH bridge would have been hard-failed by
    its own anchor -- and, read the other way, the anchor could not tell
    the two tokens apart per job."""
    milli_word = "00" * 12 + MILLI.erc20_address[2:].lower()

    # USDC-sourced attestation against a milliETH job -> terminal.
    store = new_store()
    engine, _ = build(store, watcher=FakeWatcher(_sent_msg()))
    seed(store, S.JobStatus.MESSAGE_SENT,
         columns={"receiver_ph": RECEIVER_PH.hex(), "post_tip_mojos": 4985,
                  "bridge_nonce": "00" * 31 + "07"},
         state={"asset": "milliETH"})
    out = engine.step()
    assert out["status"] == S.JobStatus.FAILED
    assert "milliETH" in (store.get_job(out["id"]).last_error or "")

    # The same job with a milliETH-sourced attestation clears the anchor.
    store2 = new_store()
    engine2, _ = build(store2, watcher=FakeWatcher(
        _sent_msg(erc20_source=milli_word,
                  contents=[milli_word, RECEIVER_PH.hex(), "1379"])))
    seed(store2, S.JobStatus.MESSAGE_SENT,
         columns={"receiver_ph": RECEIVER_PH.hex(), "post_tip_mojos": 4985,
                  "bridge_nonce": "00" * 31 + "07"},
         state={"asset": "milliETH"})
    out2 = engine2.step()
    assert out2["status"] == S.JobStatus.FUNDING_CLAIM


def test_a_usdc_job_still_rejects_a_millieth_attestation():
    """The anchor stays symmetric: the legacy asset is no less protected."""
    milli_word = "00" * 12 + MILLI.erc20_address[2:].lower()
    store = new_store()
    engine, _ = build(store, watcher=FakeWatcher(_sent_msg(
        erc20_source=milli_word)))
    seed(store, S.JobStatus.MESSAGE_SENT,
         columns={"receiver_ph": RECEIVER_PH.hex(), "post_tip_mojos": 4985,
                  "bridge_nonce": "00" * 31 + "07"})      # legacy: no asset
    out = engine.step()
    assert out["status"] == S.JobStatus.FAILED


# --------------------------------------------------------------------------- #
# Findings from the adversarial pass on this PR.
# --------------------------------------------------------------------------- #

def test_the_fee_bump_resign_rebuilds_for_the_same_asset(monkeypatch):
    """A stuck bridge is re-signed at the PINNED nonce. Rebuilding it
    without the job's asset would replace a milliETH bridge with a USDC
    one -- and the attested-source anchor would then kill the job only
    AFTER real funds had moved."""
    from gui.services.warp import evm as evm_mod

    monkeypatch.setattr(evm_mod, "sign_tx", fake_sign_tx)
    store = new_store()
    engine, ctx = build(store)
    job = seed(
        store, S.JobStatus.BRIDGING,
        columns={"receiver_ph": RECEIVER_PH.hex(), "amount_mojos": 5000,
                 "bridge_tx_hash": "0x" + "11" * 32},
        state={"asset": "milliETH", "bridge_raw": "aa" * 10,
               # nonce still unconsumed on chain, polls past the stuck
               # threshold, and a recorded fee base to escalate from --
               # the exact conditions for the re-sign branch.
               "bridge_tx_nonce": 10 ** 9,
               "bridge_unmined_polls": S._STUCK_TX_POLLS,
               "bridge_fees": [10 ** 9, 10 ** 6],
               "bridge_fee_bumps": 0},
    )
    engine._bridge_unmined_step(job)
    assert ctx.evm.bridge_asset is not None, "the re-sign passed no asset"
    assert ctx.evm.bridge_asset.symbol == "milliETH"


def test_a_non_usdc_deposit_is_refused_while_caps_are_usdc_scaled():
    """min_micros/max_micros are scaled by USDC's decimals; applying them
    to a 3-decimal token would read 1000x too permissive. B1 refuses
    rather than running uncapped -- the inertness is enforced, not just
    an absence of callers."""
    store = new_store()
    engine, _ = build(store, params=default_params(max_micros=5_000_000))
    seed(store, S.JobStatus.AWAITING_DEPOSIT, state={"asset": "milliETH"})
    out = engine.step()
    assert out["status"] == S.JobStatus.FAILED
    assert "not enabled yet" in (store.get_job(out["id"]).last_error or "")


def test_deposit_seen_freezes_the_asset_with_the_amounts():
    store = new_store()
    engine, ctx = build(store)
    ctx.evm.erc20 = 5_000_000                       # 5 USDC
    seed(store, S.JobStatus.AWAITING_DEPOSIT, state={"manual": True})
    out = engine.step()
    assert out["status"] == S.JobStatus.DEPOSIT_SEEN
    assert store.get_job(out["id"]).state["asset"] == "USDC", (
        "the asset must be frozen alongside the binding")


def test_the_claim_anchor_rejects_an_id_the_network_does_not_pin():
    """Exercises the real validator: a ClaimRequest cannot introduce a
    wrapped id this deployment does not pin, so the per-job anchor is no
    weaker than the global constant it replaced."""
    from gui.services.warp import drivers

    # Both pinned ids are accepted, and normalise to lower case.
    pinned = drivers._require_pinned_asset_id
    assert pinned(NET, USDC.expected_asset_id) == USDC.expected_asset_id.lower()
    assert pinned(NET, MILLI.expected_asset_id) == MILLI.expected_asset_id.lower()
    assert pinned(NET, MILLI.expected_asset_id.upper()) == (
        MILLI.expected_asset_id.lower()), "case is normalised"
    # An empty request falls back to the network anchor (legacy behaviour).
    assert pinned(NET, "") == NET.expected_asset_id.lower()
    # A well-formed but unpinned id is refused before any work happens.
    with pytest.raises(drivers.WarpDriverError, match="does not pin"):
        drivers._require_pinned_asset_id(NET, "ff" * 32)


def test_an_asset_below_cat_precision_is_refused_at_startup_and_in_maths():
    """10 ** negative is a float in Python; money arithmetic must never go
    floating. Caught at the refuse-to-start gate, with a hard guard in the
    conversion helper for descriptors built by hand."""
    import dataclasses

    from gui.services.warp import drivers

    coarse = dataclasses.replace(MILLI, erc20_decimals=2)   # < cat_decimals 3
    with pytest.raises(ValueError, match="CAT decimals"):
        evm._mojo_factor(NET, coarse)

    broken = dataclasses.replace(
        NET, assets=(("USDC", USDC), ("milliETH", coarse)))
    with pytest.raises(drivers.WarpDriverError, match="decimals"):
        drivers.verify_wrapped_asset_anchor(broken)


def test_an_empty_asset_field_is_terminal_not_a_usdc_default():
    """Only an ABSENT field is legacy. A present-but-empty value is
    corruption, and defaulting it would bridge as USDC."""
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT, state={"asset": ""})
    with pytest.raises(S.WarpTerminal, match="empty asset"):
        eng._job_asset(job)


def test_a_job_refuses_to_resume_if_its_assets_terms_changed():
    """The symbol is a NAME. A job frozen against one contract/precision/
    TAIL must not silently adopt a different meaning of that name."""
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT,
               state={"asset": "USDC",
                      "asset_fingerprint": "0xdead:6:" + "ff" * 32})
    with pytest.raises(S.WarpTerminal, match="terms changed"):
        eng._job_asset(job)

    # The fingerprint this build actually produces resolves cleanly.
    # (Fresh store: the job table allows only one active job.)
    store2, eng2, _ctx2 = _engine()
    ok = seed(store2, S.JobStatus.AWAITING_DEPOSIT,
              state={"asset": "USDC",
                     "asset_fingerprint": S._asset_fingerprint(USDC)})
    assert eng2._job_asset(ok).symbol == "USDC"


def test_deposit_freeze_records_the_asset_terms():
    store = new_store()
    engine, ctx = build(store)
    ctx.evm.erc20 = 5_000_000
    seed(store, S.JobStatus.AWAITING_DEPOSIT, state={"manual": True})
    out = engine.step()
    st = store.get_job(out["id"]).state
    assert st["asset"] == "USDC"
    assert st["asset_fingerprint"] == S._asset_fingerprint(USDC)


def test_the_approving_phase_uses_the_jobs_asset_for_both_calls():
    """A regression querying or approving USDC here would otherwise pass
    every other test in this file."""
    from gui.services.warp import evm as evm_mod

    store = new_store()
    engine, ctx = build(store)
    ctx.evm.allowance = 0                       # force the approve path
    seed(store, S.JobStatus.APPROVING,
         columns={"amount_usdc_micros": 5000, "amount_mojos": 5000},
         state={"asset": "milliETH"})
    import pytest as _pytest
    with _pytest.MonkeyPatch.context() as mp:
        mp.setattr(evm_mod, "sign_tx", fake_sign_tx)
        engine.step()
    assert ctx.evm.allowance_asset is not None
    assert ctx.evm.allowance_asset.symbol == "milliETH", "allowance lookup"
    assert ctx.evm.approve_asset is not None
    assert ctx.evm.approve_asset.symbol == "milliETH", "prepared approval"


def test_the_first_bridging_signature_uses_the_jobs_asset():
    """The fee-bump replacement is covered separately; this pins the FIRST
    bridgeToChia, which is the transaction that actually moves funds."""
    from gui.services.warp import evm as evm_mod

    store = new_store()
    engine, ctx = build(store)
    seed(store, S.JobStatus.BRIDGING,
         columns={"receiver_ph": RECEIVER_PH.hex(), "amount_mojos": 5000},
         state={"asset": "milliETH"})
    import pytest as _pytest
    with _pytest.MonkeyPatch.context() as mp:
        mp.setattr(evm_mod, "sign_tx", fake_sign_tx)
        engine.step()
    assert ctx.evm.bridge_asset is not None
    assert ctx.evm.bridge_asset.symbol == "milliETH"


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
