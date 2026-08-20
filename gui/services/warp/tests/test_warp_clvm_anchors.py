"""Offline correctness anchors for the warp.green CLVM layer.

These tests move no funds and hit no network. They prove that the hand-written
primitives in :mod:`gui.services.warp.clvm_utils`, composed with the vendored
puzzle reveals and the network constants, reproduce values that already exist
on-chain:

* every vendored puzzle reveal hashes to its registered tree hash (cross-checked
  against ``chia_rs`` as an independent oracle);
* the bridging-puzzle hash matches the anchor ``a09eb1ea...9037``;
* the derived wUSDC.b wrapped-asset TAIL matches the dexie-confirmed asset id
  ``fa4a180a...`` -- the anchor the live service refuses to move funds without;
* ``coin_name`` matches ``chia_rs.Coin(...).name()`` and hard-coded goldens;
* bech32m matches BIP-350 known-answer vectors (including a taproot address) and
  round-trips a Chia address.

The wrapped-asset derivation helpers below are a faithful port of warp's
``cli/drivers/wrapped_assets.py`` mint path; they will be promoted into
``drivers.py`` (Commit 5). Keeping a copy here makes this anchor self-contained.
"""

from __future__ import annotations

import pytest

# Hard dependencies of the whole bridge feature. If they are absent (e.g. a
# minimal CI image), skip rather than error -- the GUI itself boots without them.
pytest.importorskip("clvm")
chia_rs = pytest.importorskip("chia_rs")

from gui.services.warp import clvm_utils as cu  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import drivers  # noqa: E402


# --------------------------------------------------------------------------- #
# Derived module hashes (loaded + hash-asserted).
# --------------------------------------------------------------------------- #

SINGLETON_MOD_HASH = cu.puzzle_hash("singleton_top_layer_v1_1")
SINGLETON_LAUNCHER_HASH = cu.SINGLETON_LAUNCHER_PUZZLE_HASH
CAT_MOD_HASH = cu.puzzle_hash("cat_v2")
BRIDGING_PUZZLE_HASH = cu.puzzle_hash("bridging_puzzle")
CAT_MINT_AND_PAYOUT_MOD_HASH = cu.puzzle_hash("cat_mint_and_payout")
BURN_INNER_PUZZLE_MOD_HASH = cu.puzzle_hash("burn_inner_puzzle")

MESSAGE_COIN_MOD = cu.load_puzzle("message_coin")
WRAPPED_TAIL_MOD = cu.load_puzzle("wrapped_tail")
CAT_MINTER_MOD = cu.load_puzzle("cat_minter")
CAT_BURNER_MOD = cu.load_puzzle("cat_burner")
BURN_INNER_PUZZLE_MOD = cu.load_puzzle("burn_inner_puzzle")


# --------------------------------------------------------------------------- #
# Port of cli/drivers/wrapped_assets.py mint derivation (structural only).
# --------------------------------------------------------------------------- #

def _message_coin_puzzle_1st_curry(launcher_id: bytes):
    return cu.curry(
        MESSAGE_COIN_MOD,
        (SINGLETON_MOD_HASH, (launcher_id, SINGLETON_LAUNCHER_HASH)),
    )


def _cat_burner_puzzle(dest_chain: bytes, dest: bytes):
    return cu.curry(
        CAT_BURNER_MOD,
        CAT_MOD_HASH,
        BURN_INNER_PUZZLE_MOD_HASH,
        BRIDGING_PUZZLE_HASH,
        dest_chain,
        dest,
    )


def _cat_minter_puzzle(launcher_id: bytes, source_chain: bytes, source: bytes):
    return cu.curry(
        CAT_MINTER_MOD,
        cu.sha256tree(_message_coin_puzzle_1st_curry(launcher_id)),
        CAT_MOD_HASH,
        cu.sha256tree(WRAPPED_TAIL_MOD),
        CAT_MINT_AND_PAYOUT_MOD_HASH,
        cu.raw_hash(b"\x01", cu.sha256tree(_cat_burner_puzzle(source_chain, source))),
        BURN_INNER_PUZZLE_MOD_HASH,
        source_chain,
        source,
    )


def _cat_burn_inner_puzzle_first_curry(dest_chain: bytes, dest: bytes, token32: bytes):
    return cu.curry(
        BURN_INNER_PUZZLE_MOD,
        cu.sha256tree(_cat_burner_puzzle(dest_chain, dest)),
        token32,
    )


def _wrapped_tail(launcher_id: bytes, source_chain: bytes, source: bytes, token: bytes):
    if len(token) < 32:
        token = b"\x00" * (32 - len(token)) + token
    return cu.curry(
        WRAPPED_TAIL_MOD,
        cu.sha256tree(_cat_minter_puzzle(launcher_id, source_chain, source)),
        cu.sha256tree(_cat_burn_inner_puzzle_first_curry(source_chain, source, token)),
    )


def _mainnet_usdc_tail_hash() -> str:
    launcher_id = bytes.fromhex(C.MAINNET.portal_launcher_id)
    source_chain = C.MAINNET.source_chain.encode()
    bridge_addr = bytes.fromhex(C.MAINNET.erc20_bridge_address[2:])
    usdc = bytes.fromhex(C.MAINNET.usdc_address[2:])
    return cu.sha256tree(
        _wrapped_tail(launcher_id, source_chain, bridge_addr, usdc)
    ).hex()


# --------------------------------------------------------------------------- #
# Puzzle tree-hash anchors.
# --------------------------------------------------------------------------- #

def _rs_tree_hash(hex_str: str) -> str:
    return bytes(
        chia_rs.Program.from_bytes(bytes.fromhex(hex_str)).get_tree_hash()
    ).hex()


def test_every_vendored_puzzle_matches_expected_and_chia_rs():
    from gui.services.warp.puzzles.hashes import EXPECTED_TREE_HASHES

    assert len(EXPECTED_TREE_HASHES) == 12
    for name, expected in EXPECTED_TREE_HASHES.items():
        program = cu.load_puzzle(name)  # hash-asserted internally
        mine = cu.sha256tree(program).hex()
        assert mine == expected, f"{name}: sha256tree {mine} != {expected}"

        raw_hex = (cu._PUZZLE_DIR / f"{name}.clsp.hex").read_text().strip()
        assert _rs_tree_hash(raw_hex) == expected, f"{name}: chia_rs disagrees"


def test_bridging_puzzle_hash_anchor():
    assert BRIDGING_PUZZLE_HASH.hex() == C.MAINNET.bridging_puzzle_hash
    assert (
        C.MAINNET.bridging_puzzle_hash
        == "a09eb1ea8c6e83c0166801dabcf4a70d361cc7f6d89c4a46bcd400ac57719037"
    )


def test_load_puzzle_unknown_name_raises():
    with pytest.raises(KeyError):
        cu.load_puzzle("not_a_real_puzzle")


def test_load_puzzle_rejects_hash_mismatch(monkeypatch):
    monkeypatch.setitem(cu.EXPECTED_TREE_HASHES, "bridging_puzzle", "00" * 32)
    cu._puzzle_cache.pop("bridging_puzzle", None)
    with pytest.raises(ValueError):
        cu.load_puzzle("bridging_puzzle")
    cu._puzzle_cache.pop("bridging_puzzle", None)


# --------------------------------------------------------------------------- #
# The load-bearing anchor: wUSDC.b wrapped-asset id.
# --------------------------------------------------------------------------- #

def test_every_listed_asset_tail_derives_to_its_pinned_id():
    """Per-asset anchor: each WarpNet.assets entry's wrapped TAIL must
    re-derive offline to its pinned expected_asset_id. The pinned milliETH
    id is asserted against config.yaml's actual trading pair separately in
    test_millieth_pin_matches_the_enabled_trading_pair."""
    for key, spec in C.MAINNET.assets:
        derived = drivers.derive_wrapped_asset_id(
            C.MAINNET, spec.erc20_address).hex()
        assert derived == spec.expected_asset_id, key
    assert C.MAINNET.asset("USDC").expected_asset_id == (
        C.MAINNET.expected_asset_id), "USDC entry mirrors the legacy anchor"
    assert C.MAINNET.asset("milliETH").expected_asset_id == (
        "f322a205c034fe28681829fa5a2e483ac421f0952eb1292945c8db06e0a471a6")
    # The migration mirror: the table entries must match the legacy
    # scalars in address and decimals too, or old and new call sites
    # could target different contracts or unit scales.
    assert C.MAINNET.asset("USDC").erc20_address == C.MAINNET.usdc_address
    assert C.MAINNET.asset("USDC").erc20_decimals == C.MAINNET.usdc_decimals
    assert C.MAINNET.asset("milliETH").erc20_address == (
        C.MAINNET.milli_eth_address)
    assert C.MAINNET.asset("milliETH").erc20_decimals == (
        C.MAINNET.milli_eth_decimals)


def test_derivation_normalizes_the_token_prefix_and_rejects_garbage():
    """A caller passing an unprefixed hex address must derive the SAME id
    (the old [2:] slice silently dropped a byte and derived a wrong one);
    non-hex or wrong-length input raises WarpDriverError, and the per-asset
    verification names the offending asset."""
    import dataclasses

    spec = C.MAINNET.asset("milliETH")
    with_prefix = drivers.derive_wrapped_asset_id(
        C.MAINNET, spec.erc20_address)
    without = drivers.derive_wrapped_asset_id(
        C.MAINNET, spec.erc20_address[2:])
    assert with_prefix == without
    for bad in ("0xnothex", "0x1234", "zz" * 20):
        with pytest.raises(drivers.WarpDriverError):
            drivers.derive_wrapped_asset_id(C.MAINNET, bad)
    empty = dataclasses.replace(spec, erc20_address="")
    broken_empty = dataclasses.replace(
        C.MAINNET,
        assets=(("USDC", C.MAINNET.asset("USDC")), ("milliETH", empty)),
    )
    with pytest.raises(drivers.WarpDriverError, match="empty erc20_address"):
        drivers.verify_wrapped_asset_anchor(broken_empty)
    mangled = dataclasses.replace(spec, erc20_address="0xnothex")
    broken = dataclasses.replace(
        C.MAINNET,
        assets=(("USDC", C.MAINNET.asset("USDC")), ("milliETH", mangled)),
    )
    with pytest.raises(drivers.WarpDriverError, match="milliETH"):
        drivers.verify_wrapped_asset_anchor(broken)


def test_millieth_pin_matches_the_enabled_trading_pair():
    """The invariant that makes bridged milliETH tradable: the descriptor's
    pinned id equals the ACTUAL wmilliETH.b pair's base_asset_id read from
    this repo's config.yaml -- not a second hardcoded copy."""
    import yaml
    from pathlib import Path

    cfg_path = Path(__file__).resolve().parents[4] / "config.yaml"
    cfg = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    pairs = [p for p in (cfg.get("pairs") or [])
             if p.get("name") == "wmilliETH.b/XCH" and p.get("enabled")]
    assert pairs, "the ENABLED wmilliETH.b/XCH pair is missing from config.yaml"
    assert pairs[0]["base_asset_id"] == (
        C.MAINNET.asset("milliETH").expected_asset_id)


def test_anchor_verification_covers_every_listed_asset():
    """A typo'd NEW asset entry must block the engine, not surface at claim
    time: verify_wrapped_asset_anchor re-derives every assets entry."""
    import dataclasses

    bad_asset = dataclasses.replace(
        C.MAINNET.asset("milliETH"), expected_asset_id="ff" * 32)
    broken = dataclasses.replace(
        C.MAINNET,
        assets=(("USDC", C.MAINNET.asset("USDC")), ("milliETH", bad_asset)),
    )
    with pytest.raises(drivers.WarpDriverError, match="milliETH"):
        drivers.verify_wrapped_asset_anchor(broken)

    empty_asset = dataclasses.replace(
        C.MAINNET.asset("milliETH"), expected_asset_id="")
    broken2 = dataclasses.replace(
        C.MAINNET,
        assets=(("USDC", C.MAINNET.asset("USDC")), ("milliETH", empty_asset)),
    )
    with pytest.raises(drivers.WarpDriverError, match="milliETH"):
        drivers.verify_wrapped_asset_anchor(broken2)


def test_wrapped_usdc_b_tail_matches_asset_id():
    derived = _mainnet_usdc_tail_hash()
    assert derived == C.MAINNET.expected_asset_id
    assert derived == "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"


# --------------------------------------------------------------------------- #
# Integer encoding + coin_name.
# --------------------------------------------------------------------------- #

def test_int_to_bytes_canonical_encoding():
    assert cu.int_to_bytes(0) == b""
    assert cu.int_to_bytes(1) == bytes.fromhex("01")
    assert cu.int_to_bytes(1000) == bytes.fromhex("03e8")
    assert cu.int_to_bytes(1_000_000_000) == bytes.fromhex("3b9aca00")
    assert cu.int_to_bytes(0xFF) == bytes.fromhex("00ff")  # sign-bit guard
    assert cu.int_to_bytes(2**64 - 1) == bytes.fromhex("00ffffffffffffffff")


# parent + puzzle hash reused across all coin_name vectors.
_CN_PARENT = bytes.fromhex(
    "46e2bdbbcd1e372523ad4cd3c9cf4b372c389733c71bb23450f715ba5aa56d50"
)
_CN_PH = bytes.fromhex(
    "a09eb1ea8c6e83c0166801dabcf4a70d361cc7f6d89c4a46bcd400ac57719037"
)
_COIN_NAME_GOLDENS = {
    0: "e4b3c81950d1b6f9efd321f3ac954adc53c2e68c72e268d509785420a89de960",
    1: "f6c9d45345b430b6632ccf7ca6491d1d67cf2ccd856e8c28d9afceb64d4338d8",
    1000: "46b4eae77453d5131385707715c57b47df63fbb3866eaefd1a85ba4b1bf47e28",
    1_000_000_000: "7f405cd0ec8dd4e1fa917964b850c268bac3520bbc09428bb9a0cd0b9aa83a09",
    18446744073709551615: "e65d44db695ee314b83a121d282a11e0ec9ffb3e701420b1e2b3be33718e24ae",
}


def test_coin_name_matches_goldens_and_chia_rs():
    for amount, golden in _COIN_NAME_GOLDENS.items():
        mine = cu.coin_name(_CN_PARENT, _CN_PH, amount).hex()
        assert mine == golden, f"amount={amount}: {mine} != golden {golden}"
        oracle = bytes(chia_rs.Coin(_CN_PARENT, _CN_PH, amount).name()).hex()
        assert mine == oracle, f"amount={amount}: {mine} != chia_rs {oracle}"


# --------------------------------------------------------------------------- #
# bech32m (BIP-350).
# --------------------------------------------------------------------------- #

def test_bech32m_bip350_known_answers():
    # Empty-data checksum vectors from BIP-350.
    assert cu.bech32_encode("a", []) == "a1lqfn3a"
    assert cu.bech32_encode("?", []) == "?1v759aa"
    for valid in ("A1LQFN3A", "a1lqfn3a", "?1v759aa"):
        hrp, data = cu.bech32_decode(valid)
        assert hrp is not None and data == []


def test_bech32m_taproot_vector():
    # BIP-350 taproot (witness v1 = bech32m) known-answer.
    addr = "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0"
    hrp, data = cu.bech32_decode(addr)
    assert hrp == "bc"
    assert data[0] == 1  # witness version
    program = bytes(cu.convertbits(data[1:], 5, 8, False))
    assert program.hex() == (
        "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
    )


def test_bech32m_corrupted_checksum_rejected():
    addr = "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj1"  # last char flipped
    hrp, data = cu.bech32_decode(addr)
    assert hrp is None and data is None


def test_chia_address_round_trip():
    ph = bytes.fromhex(C.MAINNET.bridging_puzzle_hash)
    addr = cu.encode_puzzle_hash(ph, "xch")
    assert addr.startswith("xch1")
    assert cu.decode_puzzle_hash(addr, expected_prefix="xch") == ph
    with pytest.raises(ValueError):
        cu.decode_puzzle_hash(addr, expected_prefix="txch")


# --------------------------------------------------------------------------- #
# curry / uncurry round-trip.
# --------------------------------------------------------------------------- #

def test_curry_uncurry_round_trip():
    mod = cu.load_puzzle("bridging_puzzle")
    args = [b"\x01\x02\x03", bytes.fromhex("deadbeef" * 8), b"bse"]
    curried = cu.curry(mod, *args)
    out = cu.uncurry(curried)
    assert out is not None
    got_mod, got_args = out
    assert cu.sha256tree(got_mod) == cu.sha256tree(mod)
    assert [bytes(a.atom) for a in got_args] == args


def test_uncurry_rejects_non_curried():
    from clvm import SExp

    assert cu.uncurry(SExp.to(b"\x00")) is None
    assert cu.uncurry(SExp.to([1, 2, 3])) is None


# --------------------------------------------------------------------------- #
# Outbound (unwrap) anchors.
# --------------------------------------------------------------------------- #

def test_burn_puzzle_hash_derives_from_this_repos_drivers():
    """Design doc Q2, settled in-tree and offline.

    sha256tree of our own cat_burner curry must equal the constant that is
    also read live from ERC20Bridge.burnPuzzleHash() at gate time. This is
    the outbound analogue of the wrapped-asset anchor: a drivers or vendored
    puzzle defect fails here before any job exists.

    Known divergence, deliberately preserved: our get_cat_burner_puzzle
    applies _strip_source(destination) and upstream does not. A no-op for the
    deployed bridge address (no leading zero byte); a bridge redeploy to a
    leading-zero address would surface here as a hash mismatch, which is the
    correct failure.
    """
    from gui.services.warp import constants as C
    from gui.services.warp import drivers

    bridge = bytes.fromhex(C.MAINNET.erc20_bridge_address[2:])
    derived = cu.sha256tree(drivers.get_cat_burner_puzzle(b"bse", bridge))
    assert derived.hex() == C.MAINNET.burn_puzzle_hash


def test_eip712_domain_separator_reconstructs_from_the_fields():
    """The separator is derived, never trusted: name/version/chainId/portal
    hashed per EIP-712 must equal the anchored constant, which was validated
    end to end by recovering a real relay's six validator signatures."""
    keccak = pytest.importorskip("eth_utils").keccak
    from gui.services.warp import constants as C

    net = C.MAINNET
    type_hash = keccak(
        text="EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)"
    )
    sep = keccak(
        type_hash
        + keccak(text=net.eip712_name)
        + keccak(text=net.eip712_version)
        + net.evm_chain_id.to_bytes(32, "big")
        + b"\x00" * 12 + bytes.fromhex(net.portal_address[2:])
    )
    assert sep.hex() == net.eip712_domain_separator
