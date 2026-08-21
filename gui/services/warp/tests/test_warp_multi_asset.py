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
    PORTAL, RECEIVER_PH, FakeCoin, FakeWatcher, _seed_claiming, _sent_msg,
    build, default_params, fake_sign_tx, new_store, seed,
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
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT, state=S._asset_stamp(MILLI, NET.cat_decimals))
    spec = eng._job_asset(job)
    assert spec.symbol == "milliETH"
    assert spec.erc20_address == NET.milli_eth_address
    assert spec.erc20_decimals == 3


def test_an_unknown_name_is_terminal_like_any_other_partial_write():
    """The name is advisory now, so an unknown one is not special-cased --
    it fails as what it is: a row recording a name without its terms."""
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT, state={"asset": "DOGE"})
    with pytest.raises(S.WarpTerminal, match="without its terms"):
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
         state=S._asset_stamp(MILLI, NET.cat_decimals))
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
         state=S._asset_stamp(MILLI, NET.cat_decimals))
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
        state={**S._asset_stamp(MILLI, NET.cat_decimals), "bridge_raw": "aa" * 10,
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
    seed(store, S.JobStatus.AWAITING_DEPOSIT, state=S._asset_stamp(MILLI, NET.cat_decimals))
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


def test_a_row_with_neither_field_is_legacy_usdc():
    """Rows predating asset stamping carry neither field, and USDC was the
    only asset the pipeline could bridge when they were written."""
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT, state={})
    assert eng._job_asset(job).symbol == "USDC"


def test_a_name_without_terms_is_a_partial_write_not_a_legacy_row():
    """Every writer records terms with the name, so a name alone is
    corruption. Resolving it by name would let the job adopt edited
    contract/precision/TAIL terms -- and for a committed job could sign a
    replacement for a DIFFERENT token at the original nonce."""
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT, state={"asset": "USDC"})
    with pytest.raises(S.WarpTerminal, match="without its terms"):
        eng._job_asset(job)


def test_identity_is_the_wrapped_id_so_a_rename_cannot_strand_a_job():
    """The TAIL is derived from the token contract: it identifies the asset
    even if the table key or symbol is renamed. Resolving by NAME (as the
    first cut did) meant any re-key made every stamped job terminal."""
    import dataclasses

    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT,
               state=S._asset_stamp(MILLI, NET.cat_decimals))
    renamed = dataclasses.replace(
        NET, assets=(("USDC", USDC), ("wmilliETH.b", MILLI)))
    eng._net = renamed
    assert eng._job_asset(job).expected_asset_id == MILLI.expected_asset_id


def test_a_job_refuses_when_its_asset_is_no_longer_pinned():
    """The one genuine stop-and-fetch-an-operator case: this deployment no
    longer pins the asset the job's funds moved under."""
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT,
               state={"asset": "milliETH",
                      "asset_fingerprint": f"v1:0xdead:3:3:{'ff' * 32}"})
    with pytest.raises(S.WarpTerminal, match="no longer pins"):
        eng._job_asset(job)


def test_a_changed_precision_ratio_is_refused():
    """Every conversion is 10 ** (erc20 - cat), so freezing one half would
    let the ratio move. Both are in the fingerprint."""
    import dataclasses

    store, eng, ctx = _engine()
    fp = S._asset_fingerprint(USDC, NET.cat_decimals)
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT,
               state={"asset": "USDC", "asset_fingerprint": fp})
    eng._net = dataclasses.replace(NET, cat_decimals=NET.cat_decimals + 1)
    with pytest.raises(S.WarpTerminal, match="terms changed"):
        eng._job_asset(job)


def test_an_unreadable_stamp_fails_closed_before_funds_move():
    """Refusing costs nothing before a Base transaction exists, so an
    unreadable stamp is terminal there -- it cannot be a legacy row, since
    those carry no fingerprint at all."""
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT,
               state={"asset": "USDC", "asset_fingerprint": "v9:whatever"})
    with pytest.raises(S.WarpTerminal, match="unreadable asset stamp"):
        eng._job_asset(job)


def test_a_corrupt_precision_is_terminal_not_an_infinite_retry():
    """step() classifies unknown exceptions as RETRYABLE, so a ValueError
    escaping the parser would retry every tick forever."""
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.AWAITING_DEPOSIT,
               state={"asset": "USDC",
                      "asset_fingerprint": f"v1:0xabc:six:3:{'ff' * 32}"})
    with pytest.raises(S.WarpTerminal, match="non-integer precision"):
        eng._job_asset(job)


def test_an_unreadable_stamp_recovers_from_the_attested_contents():
    """contents[0] is the ERC-20 the validators witnessed -- durable
    on-chain evidence. A job that has it can be resumed safely even when
    its local stamp is unreadable."""
    usdc_word = "00" * 12 + USDC.erc20_address[2:].lower()
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.CLAIMING,
               state={"asset": "USDC", "asset_fingerprint": "v9:whatever",
                      "message_contents": [usdc_word, "cd" * 32, "1379"]})
    assert eng._job_asset(job).expected_asset_id == USDC.expected_asset_id


def test_without_attested_evidence_an_unreadable_stamp_stops_the_job():
    """A wrong guess moves the wrong funds, which is worse than a job that
    needs manual recovery -- so there is no name-based last resort."""
    store, eng, ctx = _engine()
    job = seed(store, S.JobStatus.BRIDGING,
               columns={"bridge_tx_hash": "0x" + "ab" * 32},
               state={"asset": "USDC", "asset_fingerprint": "v9:whatever"})
    with pytest.raises(S.WarpTerminal, match="no attested contents"):
        eng._job_asset(job)


def test_every_creation_path_stamps_the_terms_not_just_the_name():
    """All five writers, not just the manual click: a path that omitted the
    stamp would leave jobs resolvable only by name."""
    expect = S._asset_fingerprint(USDC, NET.cat_decimals)

    store, eng, ctx = _engine()
    st = store.get_job(eng.request_bridge()["id"]).state
    assert (st["asset"], st["asset_fingerprint"]) == ("USDC", expect)

    store2, eng2, ctx2 = _engine()
    st2 = store2.get_job(
        eng2.request_bridge(automatic=True)["id"]).state
    assert st2["asset_fingerprint"] == expect and st2["rebalance"] is True

    store3, eng3, ctx3 = _engine(auto_bridge=True, enabled=True, min_micros=1)
    ctx3.evm.erc20 = 5_000_000
    auto = eng3.maybe_start_auto_job()
    assert auto is not None
    assert store3.get_job(auto["id"]).state["asset_fingerprint"] == expect

    store4, eng4, ctx4 = _engine(max_unwrap_micros=10_000_000)
    st4 = store4.get_job(eng4.request_unwrap(
        5000, "0x" + "ab" * 20)["id"]).state
    assert st4["asset_fingerprint"] == expect, "outbound records it too"

    # ...and the DEPOSIT_SEEN freeze re-stamps with the same terms.
    store5 = new_store()
    eng5, ctx5 = build(store5)
    ctx5.evm.erc20 = 5_000_000
    seed(store5, S.JobStatus.AWAITING_DEPOSIT, state={"manual": True})
    out = eng5.step()
    assert store5.get_job(out["id"]).state["asset_fingerprint"] == expect


def test_the_inertness_gate_follows_identity_not_the_display_name():
    """B1 only enables the legacy USDC asset. Keying that on the SYMBOL
    would strand a live job whenever the descriptor is re-cased, and would
    wave through anything else that happened to be named USDC."""
    import dataclasses

    # Renamed/re-cased USDC still bridges: same wrapped id, same asset.
    renamed = dataclasses.replace(USDC, symbol="usdc.b")
    net2 = dataclasses.replace(NET, assets=(("usdc.b", renamed),
                                            ("milliETH", MILLI)))
    store = new_store()
    engine, ctx = build(store)
    engine._net = net2
    ctx.evm.erc20 = 5_000_000
    seed(store, S.JobStatus.AWAITING_DEPOSIT,
         state={"manual": True, **S._asset_stamp(renamed, net2.cat_decimals)})
    assert engine.step()["status"] == S.JobStatus.DEPOSIT_SEEN

    # An impostor named USDC but carrying another wrapped id is refused.
    impostor = dataclasses.replace(MILLI, symbol="USDC")
    net3 = dataclasses.replace(NET, assets=(("USDC", impostor),))
    store2 = new_store()
    engine2, ctx2 = build(store2)
    engine2._net = net3
    seed(store2, S.JobStatus.AWAITING_DEPOSIT,
         state={"manual": True, **S._asset_stamp(impostor, net3.cat_decimals)})
    out = engine2.step()
    assert out["status"] == S.JobStatus.FAILED
    assert "not enabled yet" in (store2.get_job(out["id"]).last_error or "")


def test_the_claiming_path_forwards_the_jobs_wrapped_id(monkeypatch):
    """The claim's per-job anchor is only worth having if it is actually
    threaded: dropping the expected_asset_id argument would leave the rest
    of the suite green while every non-USDC job failed AFTER its Base funds
    had moved and its Chia claim was funded."""
    from types import SimpleNamespace

    from gui.services.warp import claim as claim_mod

    coin = FakeCoin(bytes([0x5a]) * 32)
    monkeypatch.setattr(claim_mod, "find_security_coin", lambda *a, **k: coin)
    monkeypatch.setattr(
        claim_mod, "sync_portal",
        lambda coinset, net, *, hint, max_hops=64: SimpleNamespace(
            coin_id=PORTAL),
    )
    monkeypatch.setattr(claim_mod, "claim_landed", lambda coinset, cid: True)

    seen = {}

    def fake_push(coinset, net, **kw):
        seen.update(kw)
        return SimpleNamespace(
            claim=SimpleNamespace(final_cat_coin_id=bytes([0xfe]) * 32),
            accepted=True, status="accepted",
        )

    monkeypatch.setattr(claim_mod, "build_and_push_claim", fake_push)

    store = new_store()
    engine, ctx = build(store)
    _seed_claiming(store, **S._asset_stamp(MILLI, NET.cat_decimals))
    ctx.coinset.record = SimpleNamespace(confirmed_block_index=100)
    engine.step()

    assert seen.get("expected_asset_id") == MILLI.expected_asset_id, (
        "the claim must be anchored to THIS job's wrapped asset")
    # ...and a USDC job forwards its own, not milliETH's.
    seen.clear()
    store2 = new_store()
    engine2, ctx2 = build(store2)
    _seed_claiming(store2, **S._asset_stamp(USDC, NET.cat_decimals))
    ctx2.coinset.record = SimpleNamespace(confirmed_block_index=100)
    engine2.step()
    assert seen.get("expected_asset_id") == USDC.expected_asset_id


def test_the_claim_builder_accepts_a_millieth_anchor():
    """The forwarded id must also be one build_claim_bundle will accept,
    or the threading above would merely relocate the failure."""
    from gui.services.warp import drivers

    assert drivers._require_pinned_asset_id(NET, MILLI.expected_asset_id) == (
        MILLI.expected_asset_id.lower())


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
