"""Offline tests for the warp.green claim/mint drivers.

No network, no node, no funds. These prove the structural correctness of the
five-spend claim bundle:

* ``curry_hashes`` (used to predict the receiver's final CAT coin id from a
  puzzle hash alone) matches real ``curry`` + ``sha256tree`` for both atom and
  Program-valued arguments;
* the ephemeral security-coin signature actually verifies under BLS against the
  curried pubkey and the AGG_SIG data the p2 puzzle will reconstruct on-chain;
* ``build_claim_bundle`` emits five spends in canonical push_tx JSON, with the
  predicted coin-id chain wired correctly (security -> minter -> eve CAT ->
  final CAT);
* the fail-closed anchors refuse to build on a stale portal, a destination that
  is not the derived cat_minter, or a mis-sized funding coin;
* the validator digest round-trips through sign/verify with a test key;
* ``build_sweep_bundle`` recovers the funding coin.

The load-bearing wUSDC.b asset-id anchor (``fa4a180a...``) is exercised
implicitly: a request built from mainnet constants only survives
``build_claim_bundle`` because the derived wrapped-TAIL matches it.
"""

from __future__ import annotations

import pytest

pytest.importorskip("clvm")
chia_rs = pytest.importorskip("chia_rs")

from chia_rs import AugSchemeMPL, G2Element  # noqa: E402

from gui.services.warp import clvm_utils as cu  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import drivers as d  # noqa: E402

SExp = cu.SExp
NET = C.MAINNET


# --------------------------------------------------------------------------- #
# Fixtures: a fully-valid claim request built from mainnet constants.
# --------------------------------------------------------------------------- #

_TOKEN_AMOUNT = 1000  # 1 USDC in CAT mojos
_CLAIM_FEE = 100_000_000
_EPHEMERAL_SEED = bytes(range(32))
_VALIDATOR_SEED = bytes(range(1, 33))
_RECEIVER_PH = bytes.fromhex(NET.bridging_puzzle_hash)  # stand-in bot xch ph
_NONCE = bytes.fromhex("11" * 32)


def _agg_extra() -> bytes:
    return bytes.fromhex(NET.agg_sig_extra_data)


def _ephemeral_sk_bytes() -> bytes:
    return bytes(AugSchemeMPL.key_gen(_EPHEMERAL_SEED))


def _contents():
    erc20 = b"\x00" * 12 + bytes.fromhex(NET.usdc_address[2:])  # padded to 32
    amount_b32 = _TOKEN_AMOUNT.to_bytes(32, "big")
    return [erc20, _RECEIVER_PH, amount_b32]


def _security_coin(amount: int | None = None) -> d.Coin:
    sk = AugSchemeMPL.key_gen(_EPHEMERAL_SEED)
    puzzle = d.p2_puzzle_for_pk(bytes(sk.get_g1()))
    ph = cu.sha256tree(puzzle)
    return d.Coin(bytes.fromhex("22" * 32), ph, amount if amount is not None else _TOKEN_AMOUNT + _CLAIM_FEE)


def _portal_coin(last_chains_and_nonces=()):
    """Fabricate a portal coin whose puzzle hash matches the reconstruction."""
    launcher_id = bytes.fromhex(NET.portal_launcher_id)
    update_ph = d.get_portal_update_puzzle_hash(NET)
    inner = d.get_portal_receiver_inner_puzzle(
        launcher_id,
        NET.signature_threshold,
        NET.validator_bls_keys,
        update_ph,
        last_chains_and_nonces,
    )
    full = d.puzzle_for_singleton(launcher_id, inner)
    return d.Coin(bytes.fromhex("33" * 32), cu.sha256tree(full), 1)


def _destination() -> bytes:
    launcher_id = bytes.fromhex(NET.portal_launcher_id)
    minter = d.get_cat_minter_puzzle(
        launcher_id, NET.source_chain.encode(), bytes.fromhex(NET.erc20_bridge_address[2:])
    )
    return cu.sha256tree(minter)


def _make_request(**overrides) -> d.ClaimRequest:
    base = dict(
        net=NET,
        portal_coin=_portal_coin(),
        portal_parent_parent_info=bytes.fromhex("44" * 32),
        portal_parent_inner_puzzle_hash=bytes.fromhex("55" * 32),
        last_chains_and_nonces=(),
        validator_sigs=(),
        sig_switches=[False] * 10,
        nonce=_NONCE,
        source=bytes.fromhex(NET.erc20_bridge_address[2:]),
        destination=_destination(),
        contents=_contents(),
        security_coin=_security_coin(),
        ephemeral_sk=_ephemeral_sk_bytes(),
        claim_fee=_CLAIM_FEE,
    )
    base.update(overrides)
    return d.ClaimRequest(**base)


# --------------------------------------------------------------------------- #
# curry_hashes correctness (predicts the final CAT coin id).
# --------------------------------------------------------------------------- #

def test_curry_hashes_matches_real_curry_atom_args():
    mod = cu.load_puzzle("cat_mint_and_payout")
    mod_hash = cu.sha256tree(mod)
    arg0, arg1 = b"\x01\x02\x03", b"\xaa" * 32
    real = cu.sha256tree(cu.curry(mod, arg0, arg1))
    predicted = d.curry_hashes(mod_hash, d._h_atom(arg0), d._h_atom(arg1))
    assert predicted == real


def test_curry_hashes_matches_real_curry_program_arg():
    # The exact shape used for the final receiver CAT: curry(CAT_MOD,
    # CAT_MOD_HASH, tail_hash, inner_puzzle) where inner is itself a Program.
    tail_hash = bytes.fromhex(NET.expected_asset_id)
    inner = d.get_cat_mint_and_payout_inner_puzzle(_RECEIVER_PH)
    real = cu.sha256tree(d.construct_cat_puzzle(tail_hash, inner))
    predicted = d.curry_hashes(
        d.CAT_MOD_HASH, d._h_atom(d.CAT_MOD_HASH), d._h_atom(tail_hash), cu.sha256tree(inner)
    )
    assert predicted == real


def test_curry_hashes_matches_real_curry_zero_args():
    mod = cu.load_puzzle("bridging_puzzle")
    assert d.curry_hashes(cu.sha256tree(mod)) == cu.sha256tree(cu.curry(mod))


# --------------------------------------------------------------------------- #
# Security coin signing.
# --------------------------------------------------------------------------- #

def test_security_coin_sig_verifies_under_bls():
    sk = AugSchemeMPL.key_gen(_EPHEMERAL_SEED)
    pk = sk.get_g1()
    coin = _security_coin()
    conditions = [
        [d.ASSERT_CONCURRENT_SPEND, bytes.fromhex("ab" * 32)],
        [d.CREATE_COIN, bytes.fromhex("cd" * 32), _TOKEN_AMOUNT],
        [d.RESERVE_FEE, _CLAIM_FEE],
    ]
    spend, sig = d.build_security_coin_spend(coin, conditions, bytes(sk), _agg_extra())

    delegated = SExp.to((1, conditions))
    data = cu.sha256tree(delegated) + coin.name() + _agg_extra()
    assert AugSchemeMPL.verify(pk, data, G2Element.from_bytes(sig))

    # Spend serialises to canonical push_tx JSON.
    js = spend.to_json()
    assert js["coin"]["amount"] == _TOKEN_AMOUNT + _CLAIM_FEE
    assert js["puzzle_reveal"].startswith("0x")
    assert js["solution"].startswith("0x")


def test_security_coin_spend_rejects_wrong_key():
    other = AugSchemeMPL.key_gen(bytes(range(9, 41)))
    coin = _security_coin()  # keyed to _EPHEMERAL_SEED
    with pytest.raises(d.WarpDriverError, match="ephemeral key"):
        d.build_security_coin_spend(coin, [[d.RESERVE_FEE, 1]], bytes(other), _agg_extra())


# --------------------------------------------------------------------------- #
# get_sigs_switch bitmask.
# --------------------------------------------------------------------------- #

def test_get_sigs_switch_vectors():
    assert d.get_sigs_switch([]) == 0
    assert d.get_sigs_switch([True]) == 1
    assert d.get_sigs_switch([False, True]) == 2
    assert d.get_sigs_switch([True, False, True]) == 5
    # 6-of-11: first six validators set.
    six = [True] * 6 + [False] * 5
    assert d.get_sigs_switch(six) == 0b111111


# --------------------------------------------------------------------------- #
# Validator digest round-trip.
# --------------------------------------------------------------------------- #

def test_validator_digest_sign_verify_round_trip():
    sk = AugSchemeMPL.key_gen(_VALIDATOR_SEED)
    portal_id = _portal_coin().name()
    digest = d.get_validator_message_digest(
        NET.source_chain.encode(),
        _NONCE,
        bytes.fromhex(NET.erc20_bridge_address[2:]),
        _destination(),
        _contents(),
        portal_id,
        _agg_extra(),
    )
    assert len(digest) == 32 + 32 + len(_agg_extra())
    sig = AugSchemeMPL.sign(sk, digest)
    assert AugSchemeMPL.verify(sk.get_g1(), digest, sig)
    # Deterministic.
    again = d.get_validator_message_digest(
        NET.source_chain.encode(), _NONCE, bytes.fromhex(NET.erc20_bridge_address[2:]),
        _destination(), _contents(), portal_id, _agg_extra(),
    )
    assert again == digest


# --------------------------------------------------------------------------- #
# Full claim bundle.
# --------------------------------------------------------------------------- #

def test_build_claim_bundle_shape_and_id_chain():
    req = _make_request()
    result = d.build_claim_bundle(req)
    bundle = result.bundle

    # Five spends, canonical order.
    assert len(bundle.coin_spends) == 5
    js = bundle.to_json()
    assert set(js) == {"coin_spends", "aggregated_signature"}
    assert js["aggregated_signature"].startswith("0x")
    for cs in js["coin_spends"]:
        assert cs["coin"]["parent_coin_info"].startswith("0x")
        assert cs["puzzle_reveal"].startswith("0x")
        assert cs["solution"].startswith("0x")

    # Predicted coin-id parentage chain.
    assert result.security_coin_id == req.security_coin.name()
    minter_parent = bytes.fromhex(js["coin_spends"][2]["coin"]["parent_coin_info"][2:])
    assert minter_parent == result.security_coin_id
    eve_parent = bytes.fromhex(js["coin_spends"][3]["coin"]["parent_coin_info"][2:])
    assert eve_parent == result.minter_coin_id

    # Final CAT id derived independently via curry_hashes.
    tail_hash = bytes.fromhex(NET.expected_asset_id)
    final_ph = d.curry_hashes(
        d.CAT_MOD_HASH, d._h_atom(d.CAT_MOD_HASH), d._h_atom(tail_hash), _RECEIVER_PH
    )
    assert result.final_cat_coin_id == cu.coin_name(result.eve_cat_coin_id, final_ph, _TOKEN_AMOUNT)


def test_message_coin_solution_rebuilds_the_portal_coin_id():
    """The message coin's puzzle ASSERT_MY_PARENT_IDs the coin rebuilt from
    the solution's (parent_parent_info . parent_inner_puzzle_hash) pair --
    and its actual parent is the CURRENT portal coin.  The pair must
    therefore be (portal.parent_coin_info, portal's own inner hash); the
    original code passed the portal's GRANDPARENT id (the field that
    belongs in the portal spend's own lineage proof), asserting a parent
    that never existed.  Every live claim failed deterministically with
    ASSERT_MY_PARENT_ID_FAILED (first seen 2026-08-23, the stuck $5 job:
    the shape tests never related this solution to the portal's parentage,
    and the fixture's 0x44... grandparent made the slip invisible).
    Mirrors the puzzle's own reconstruction end-to-end."""
    req = _make_request()
    result = d.build_claim_bundle(req)
    js = result.bundle.to_json()

    # Find the message-coin spend by its parentage (order-independent).
    message_spends = [
        cs for cs in js["coin_spends"]
        if bytes.fromhex(cs["coin"]["parent_coin_info"][2:]) == req.portal_coin.name()
    ]
    assert len(message_spends) == 1
    solution = cu.program_from_hex(message_spends[0]["solution"][2:])
    _receiver_pair, parent_pair, _msg_id = list(solution.as_iter())
    pp = parent_pair.first().as_atom()
    pih = parent_pair.rest().as_atom()

    # The pair describes the portal coin itself, not its grandparent.
    assert pp == req.portal_coin.parent_coin_info
    launcher_id = bytes.fromhex(NET.portal_launcher_id)
    update_ph = d.get_portal_update_puzzle_hash(NET)
    inner = d.get_portal_receiver_inner_puzzle(
        launcher_id,
        NET.signature_threshold,
        NET.validator_bls_keys,
        update_ph,
        (),
    )
    assert pih == cu.sha256tree(inner)

    # End-to-end: the puzzle's reconstruction lands on the actual parent.
    outer_ph = cu.sha256tree(d.puzzle_for_singleton(launcher_id, inner))
    assert outer_ph == req.portal_coin.puzzle_hash
    assert cu.coin_name(pp, outer_ph, 1) == req.portal_coin.name()


def test_claim_bundle_aggregated_signature_signs_security_coin():
    # With no validator sigs, the aggregate is exactly the security-coin sig.
    req = _make_request()
    result = d.build_claim_bundle(req)

    conditions = [
        [d.ASSERT_CONCURRENT_SPEND, result.eve_cat_coin_id],
        [d.CREATE_COIN, req.destination, _TOKEN_AMOUNT],
        [d.RESERVE_FEE, _CLAIM_FEE],
    ]
    data = cu.sha256tree(SExp.to((1, conditions))) + req.security_coin.name() + _agg_extra()
    pk = AugSchemeMPL.key_gen(_EPHEMERAL_SEED).get_g1()
    sig = G2Element.from_bytes(result.bundle.aggregated_signature)
    assert AugSchemeMPL.verify(pk, data, sig)


def test_claim_bundle_minter_is_plain_xch_of_post_tip():
    result = d.build_claim_bundle(_make_request())
    minter_spend = result.bundle.coin_spends[2]
    assert minter_spend.coin.amount == _TOKEN_AMOUNT
    assert minter_spend.coin.puzzle_hash == _destination()


# --------------------------------------------------------------------------- #
# Fail-closed anchors.
# --------------------------------------------------------------------------- #

def test_refuses_stale_portal():
    bad_portal = d.Coin(bytes.fromhex("33" * 32), bytes.fromhex("00" * 32), 1)
    with pytest.raises(d.WarpDriverError, match="portal"):
        d.build_claim_bundle(_make_request(portal_coin=bad_portal))


def test_refuses_wrong_destination():
    with pytest.raises(d.WarpDriverError, match="destination"):
        d.build_claim_bundle(_make_request(destination=bytes.fromhex("de" * 32)))


def test_refuses_mis_sized_security_coin():
    with pytest.raises(d.WarpDriverError, match="security coin amount"):
        d.build_claim_bundle(_make_request(security_coin=_security_coin(amount=123)))


def test_refuses_non_positive_amount():
    contents = [_contents()[0], _RECEIVER_PH, (0).to_bytes(32, "big")]
    with pytest.raises(d.WarpDriverError, match="positive"):
        d.build_claim_bundle(_make_request(contents=contents, security_coin=_security_coin(amount=_CLAIM_FEE)))


def test_refuses_wrong_contents_length():
    with pytest.raises(d.WarpDriverError, match="3 message contents"):
        d.build_claim_bundle(_make_request(contents=[b"\x00" * 32, b"\x11" * 32]))


# --------------------------------------------------------------------------- #
# Sweep.
# --------------------------------------------------------------------------- #

def test_build_sweep_bundle_recovers_funding_coin():
    coin = _security_coin()
    dest = bytes.fromhex("77" * 32)
    bundle = d.build_sweep_bundle(NET, coin, dest, _ephemeral_sk_bytes(), sweep_fee=500)

    assert len(bundle.coin_spends) == 1
    spend = bundle.coin_spends[0]
    # CREATE_COIN(dest, amount-fee) present in the delegated conditions.
    conditions = [[d.CREATE_COIN, dest, coin.amount - 500], [d.RESERVE_FEE, 500]]
    data = cu.sha256tree(SExp.to((1, conditions))) + coin.name() + _agg_extra()
    pk = AugSchemeMPL.key_gen(_EPHEMERAL_SEED).get_g1()
    assert AugSchemeMPL.verify(pk, data, G2Element.from_bytes(bundle.aggregated_signature))
    assert spend.to_json()["coin"]["amount"] == coin.amount


def test_sweep_rejects_fee_exceeding_amount():
    coin = _security_coin(amount=100)
    with pytest.raises(d.WarpDriverError, match="sweep fee"):
        d.build_sweep_bundle(NET, coin, bytes.fromhex("77" * 32), _ephemeral_sk_bytes(), sweep_fee=100)
