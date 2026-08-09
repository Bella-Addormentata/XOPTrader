"""Burn-bundle drivers pinned against a real mainnet unwrap.

``fixtures_unwrap.json`` was captured live (2026-08-09) with the repo's own
clients: the delivered relay transaction on Base and the Chia-side lineage --
security coin -> cat_burner coin -> bridging coin -- including the actual
puzzle reveals and solutions. Everything here is offline and deterministic;
the fixture is the ground truth these constructions must reproduce.

The one thing no fixture can prove is the daemon-side ``cat_spend`` -- the
true commit point -- which has never been executed anywhere. That is the
micro-unwrap rehearsal's job, not a unit test's.
"""

from __future__ import annotations

import json
import pathlib

import pytest

pytest.importorskip("clvm")
pytest.importorskip("chia_rs")

from gui.services.warp import clvm_utils as cu  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import drivers  # noqa: E402
from gui.services.warp.drivers import BurnRequest, Coin, build_burn_bundle  # noqa: E402

NET = C.MAINNET
FX = json.loads(
    (pathlib.Path(__file__).parent / "fixtures_unwrap.json").read_text()
)


def _hx(s: str) -> bytes:
    return bytes.fromhex(s[2:] if s.startswith("0x") else s)


def _prog(hex_str: str):
    return cu.program_from_hex(hex_str[2:] if hex_str.startswith("0x") else hex_str)


def _fx_coin(d: dict) -> Coin:
    return Coin(_hx(d["parent_coin_info"]) if "parent_coin_info" in d else _hx(d["parent"]),
                _hx(d["puzzle_hash"]), int(d["amount"]))


# --------------------------------------------------------------------------- #
# The offline nonce and the coin chain.
# --------------------------------------------------------------------------- #

def test_the_real_lineage_reproduces_from_our_constructions():
    """security -> cat_burner -> bridging, every id byte-equal to mainnet.

    This is the design doc's central operational claim: the nonce (the
    bridging coin's id) is a pure function of the security coin, so it is
    computable offline BEFORE anything is pushed -- a resumed job never
    waits to be told what to poll for.
    """
    toll = NET.chia_toll_mojos
    security = _fx_coin(FX["security_spend"]["coin"])
    assert security.amount == toll

    assert security.name().hex() == FX["security_coin_id"]

    cat_burner = Coin(security.name(), _hx(NET.burn_puzzle_hash), toll)
    assert cat_burner.name().hex() == FX["cat_burner_coin_id"]

    bridging = Coin(cat_burner.name(), _hx(NET.bridging_puzzle_hash), toll)
    assert bridging.name().hex() == FX["nonce"], "nonce == bridging coin id"


def test_the_real_message_source_is_the_burn_puzzle_hash():
    """The relay's `source` slot is the cat_burner PUZZLE HASH -- never a
    coin id. Building the digest from a coin id yields one no signature
    verifies against, stalling the job in COLLECTING_EVM_SIGS forever."""
    assert FX["source"] == NET.burn_puzzle_hash
    assert FX["bridging_coin"]["puzzle_hash"] == NET.bridging_puzzle_hash


# --------------------------------------------------------------------------- #
# The real cat_burner spend, reproduced field by field.
# --------------------------------------------------------------------------- #

def _real_cat_burner_solution_fields() -> list:
    sol = _prog(FX["cat_burner_spend"]["solution"])
    return list(sol.as_iter())


def test_our_cat_burner_solution_matches_the_real_one():
    """The captured unwrap is a third-party token (an 18-decimal CAT at
    0x39916e...), not wUSDC.b -- which makes it the BETTER golden: only the
    deployment constants (bridge, toll, burn hash) are shared, so matching it
    proves the builders generalize rather than encoding one token's values.
    The token, receiver and TAIL tree-hash come from the real solution."""
    fields = _real_cat_burner_solution_fields()
    assert len(fields) == 8

    cat_burner = _fx_coin(FX["cat_burner_spend"]["coin"])
    ours = drivers.get_cat_burner_puzzle_solution_prehashed(
        bytes(fields[0].atom),           # source CAT coin id
        bytes(fields[1].atom),           # sha256(0x01 || tail_hash)
        fields[2].as_int(),
        bytes(fields[3].atom),           # token32 (already padded on chain)
        bytes(fields[4].atom),           # receiver
        cat_burner,
    )
    real = _prog(FX["cat_burner_spend"]["solution"])
    assert cu.sha256tree(ours) == cu.sha256tree(real)

    # And the real toll slot equals our constant.
    assert fields[5].as_int() == NET.chia_toll_mojos


def test_our_cat_burner_puzzle_matches_the_real_reveal():
    ours = drivers.get_cat_burner_puzzle(b"bse", _hx(NET.erc20_bridge_address))
    real = _prog(FX["cat_burner_spend"]["puzzle_reveal"])
    assert cu.sha256tree(ours) == cu.sha256tree(real)


def test_the_burn_cat_coin_id_rederives_through_our_curry_chain():
    """The announcement interlock: cat_burner recomputes the burn CAT's id
    from its solution fields via the curry-hash chain. Our chain must land on
    the same coin id, or a bundle we build can never satisfy the real
    puzzle's announcement."""
    fields = _real_cat_burner_solution_fields()
    cat_parent_info = bytes(fields[0].atom)
    tail_treehash = bytes(fields[1].atom)
    cat_amount = fields[2].as_int()
    token32 = bytes(fields[3].atom)
    receiver = bytes(fields[4].atom)

    # We do not hold this token's TAIL preimage, only its tree hash -- which
    # is all the puzzle itself ever sees. Mirror its derivation: the burn CAT
    # outer hash via curry_hashes over (CAT_MOD_HASH, tail, inner) tree
    # hashes, with the inner built by OUR second-curry constructor.
    inner = drivers.get_cat_burn_inner_puzzle(
        b"bse",
        _hx(NET.erc20_bridge_address),
        token32,
        receiver,
        NET.chia_toll_mojos,
    )
    outer_hash = drivers.curry_hashes(
        drivers.CAT_MOD_HASH,
        cu.raw_hash(b"\x01", drivers.CAT_MOD_HASH),
        tail_treehash,
        cu.sha256tree(inner),
    )
    burn_cat_id = cu.coin_name(cat_parent_info, outer_hash, cat_amount)

    # Run the REAL cat_burner bytecode with the REAL solution and find the
    # coin id inside its ASSERT_COIN_ANNOUNCEMENT-adjacent output: the id it
    # re-derived must equal ours.
    from clvm import run_program
    from clvm.operators import OPERATOR_LOOKUP

    puzzle = _prog(FX["cat_burner_spend"]["puzzle_reveal"])
    solution = _prog(FX["cat_burner_spend"]["solution"])
    _cost, conditions = run_program(puzzle, solution, OPERATOR_LOOKUP)
    atoms = []
    for cond in conditions.as_iter():
        atoms.extend(bytes(a.atom) for a in cond.as_iter() if a.atom is not None)
    joined = b"|".join(atoms)
    assert burn_cat_id in joined or any(
        cu.raw_hash(burn_cat_id, bytes(x)) in atoms or burn_cat_id == x
        for x in atoms
    ) or any(burn_cat_id in a for a in atoms), (
        "the real puzzle's announcement chain does not contain our derived "
        "burn CAT coin id -- the curry chain diverges"
    )


# --------------------------------------------------------------------------- #
# The real security spend.
# --------------------------------------------------------------------------- #

def test_the_real_security_conditions_are_what_we_emit():
    """CREATE_COIN(cat_burner_ph, toll) + ASSERT_CONCURRENT_SPEND(cat_burner_id),
    from the p2_delegated public branch -- byte-decoded from the real spend."""
    sol = _prog(FX["security_spend"]["solution"])
    parts = list(sol.as_iter())
    delegated = parts[1]
    # (q . conditions)
    conditions = list(delegated.as_pair()[1].as_iter())
    opcodes = [c.as_pair()[0].as_int() for c in conditions]
    assert drivers.CREATE_COIN in opcodes
    assert drivers.ASSERT_CONCURRENT_SPEND in opcodes

    by_op = {c.as_pair()[0].as_int(): list(c.as_iter())[1:] for c in conditions}
    create = by_op[drivers.CREATE_COIN]
    assert bytes(create[0].atom).hex() == NET.burn_puzzle_hash
    assert create[1].as_int() == NET.chia_toll_mojos
    concurrent = by_op[drivers.ASSERT_CONCURRENT_SPEND]
    assert bytes(concurrent[0].atom).hex() == FX["cat_burner_coin_id"]


# --------------------------------------------------------------------------- #
# build_burn_bundle end to end (synthetic coins, real-shape assertions).
# --------------------------------------------------------------------------- #

def _synthetic_request():
    from gui.services.warp import keystore

    bls = keystore.new_bls_key()
    security_ph = cu.sha256tree(drivers.p2_puzzle_for_pk(bls.public_key))
    security = Coin(b"\x11" * 32, security_ph, NET.chia_toll_mojos)

    receiver = _hx("0x" + "ab" * 20)
    inner = drivers.get_cat_burn_inner_puzzle(
        b"bse", _hx(NET.erc20_bridge_address), _hx(NET.usdc_address),
        receiver, NET.chia_toll_mojos,
    )
    outer_hash = cu.sha256tree(
        drivers.construct_cat_puzzle(_hx(NET.expected_asset_id), inner)
    )
    burn_cat = Coin(b"\x22" * 32, outer_hash, 5000)
    lineage = [b"\x33" * 32, b"\x44" * 32, 5000]
    return BurnRequest(
        net=NET, burn_cat_coin=burn_cat, cat_lineage_proof=lineage,
        security_coin=security, security_sk=bls.private_key, receiver=receiver,
    ), burn_cat


def test_build_burn_bundle_shape_and_offline_nonce():
    req, burn_cat = _synthetic_request()
    out = build_burn_bundle(req)

    assert len(out.bundle.coin_spends) == 4
    assert out.cat_burner_coin.parent_coin_info == req.security_coin.name()
    assert out.cat_burner_coin.puzzle_hash.hex() == NET.burn_puzzle_hash
    assert out.bridging_coin.parent_coin_info == out.cat_burner_coin.name()
    assert out.nonce == out.bridging_coin.name()

    # Determinism: a resumed job rebuilding the bundle gets the same nonce.
    assert build_burn_bundle(req).nonce == out.nonce

    # The burn CAT spend is ring-of-one with extra_delta == -amount.
    cat_spend = out.bundle.coin_spends[1]
    fields = list(cat_spend.solution.as_iter())
    assert fields[6].as_int() == -burn_cat.amount
    assert bytes(fields[2].atom) == burn_cat.name()


def test_build_burn_bundle_refuses_a_wrong_security_amount():
    req, _ = _synthetic_request()
    import dataclasses
    bad = dataclasses.replace(
        req, security_coin=Coin(b"\x11" * 32, req.security_coin.puzzle_hash, 999)
    )
    with pytest.raises(drivers.WarpDriverError, match="toll"):
        build_burn_bundle(bad)


def test_build_burn_bundle_refuses_a_misplaced_burn_cat():
    req, _ = _synthetic_request()
    import dataclasses
    bad = dataclasses.replace(
        req, burn_cat_coin=Coin(b"\x22" * 32, b"\x55" * 32, 5000)
    )
    with pytest.raises(drivers.WarpDriverError, match="not at the expected"):
        build_burn_bundle(bad)


def test_a_20_byte_token_and_a_padded_token_curry_identically():
    """The pad rule: 20-byte input must be treated as its 32-byte form --
    proven from the real inner puzzle, whose curried arg is the padded hash."""
    r = _hx("0x" + "ab" * 20)
    a = drivers.get_cat_burn_inner_puzzle(
        b"bse", _hx(NET.erc20_bridge_address), _hx(NET.usdc_address), r, 10)
    b = drivers.get_cat_burn_inner_puzzle(
        b"bse", _hx(NET.erc20_bridge_address),
        b"\x00" * 12 + _hx(NET.usdc_address), r, 10)
    assert cu.sha256tree(a) == cu.sha256tree(b)


def test_swapped_parent_ids_are_caught_by_width_or_diverge():
    """The two parent fields are an easy swap; at minimum the solutions must
    reject non-32-byte ids so a shape mistake cannot slip through silently."""
    with pytest.raises(drivers.WarpDriverError):
        drivers.get_burn_inner_puzzle_solution(b"\x01" * 20, b"\x02" * 32, 0)
    with pytest.raises(drivers.WarpDriverError):
        drivers.get_burn_inner_puzzle_solution(b"\x01" * 32, b"\x02" * 20, 0)
