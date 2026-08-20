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
    RECEIVER_PH, FakeWatcher, _sent_msg, build, default_params, new_store, seed,
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


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
