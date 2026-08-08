"""Pure-Python CLVM helpers for the warp.green bridge.

The warp bridge only ever *constructs* CLVM structures (puzzles, solutions, coin
ids) and hashes them -- it never has to *evaluate* CLVM. That lets us avoid the
heavyweight ``chia-blockchain`` dependency entirely and lean on:

* ``clvm`` (pure Python) for :class:`clvm.SExp` and (de)serialization, and
* a handful of hand-written, individually anchored primitives below
  (``sha256tree``, ``curry``, ``uncurry``, ``coin_name``, bech32m).

Every primitive here is verified against on-chain golden values in
``tests/test_warp_clvm_anchors.py`` (the derived wUSDC.b TAIL, real ``coin_name``
vectors from ``chia_rs.Coin``, and BIP-350 bech32m vectors), so a regression in
this file fails the offline test suite rather than moving funds incorrectly.

``clvm`` is imported at module top because this module is meaningless without it;
it is only ever imported lazily by warp code paths, so the GUI still boots with
the bridge disabled or its optional dependencies absent.
"""

from __future__ import annotations

import hashlib
import io
from pathlib import Path
from typing import Iterable, Optional, Sequence, Tuple

from clvm import SExp
from clvm.serialize import sexp_from_stream

from .puzzles.hashes import EXPECTED_TREE_HASHES

# --------------------------------------------------------------------------- #
# Well-known chia protocol constants (network-independent).
# --------------------------------------------------------------------------- #

# Puzzle hash of the singleton launcher (singleton_launcher.clsp). Stable across
# mainnet/testnet; used when currying the message-coin and singleton puzzles.
SINGLETON_LAUNCHER_PUZZLE_HASH = bytes.fromhex(
    "eff07522495060c066f66f32acc2a77e3a3e737aca8baea4d1a64ea4cdc13da9"
)

_PUZZLE_DIR = Path(__file__).resolve().parent / "puzzles"

# CLVM operator atoms used when building the curry / uncurry envelope.
_OP_APPLY = b"\x02"  # a
_OP_QUOTE = b"\x01"  # q
_OP_CONS = b"\x04"   # c


# --------------------------------------------------------------------------- #
# Serialization.
# --------------------------------------------------------------------------- #

def program_from_bytes(blob: bytes) -> SExp:
    """Deserialize a CLVM program from its canonical byte encoding."""
    return sexp_from_stream(io.BytesIO(blob), SExp.to)


def program_from_hex(hex_str: str) -> SExp:
    """Deserialize a CLVM program from a hex string (whitespace tolerated)."""
    return program_from_bytes(bytes.fromhex(hex_str.strip()))


def program_to_bytes(program) -> bytes:
    """Serialize a CLVM program to its canonical byte encoding."""
    return SExp.to(program).as_bin()


# --------------------------------------------------------------------------- #
# Hashing.
# --------------------------------------------------------------------------- #

def sha256(*blobs: bytes) -> bytes:
    """SHA-256 over the concatenation of ``blobs`` (no separators)."""
    h = hashlib.sha256()
    for blob in blobs:
        h.update(blob)
    return h.digest()


# ``raw_hash`` is the name warp's cli/drivers use for this; keep it as an alias so
# the ported derivation reads identically to the reference implementation.
raw_hash = sha256


def sha256tree(program) -> bytes:
    """CLVM tree hash: sha256(0x02 . left . right) for pairs, sha256(0x01 . atom)."""
    node = SExp.to(program)
    pair = node.pair
    if pair is not None:
        return hashlib.sha256(
            b"\x02" + sha256tree(pair[0]) + sha256tree(pair[1])
        ).digest()
    return hashlib.sha256(b"\x01" + node.atom).digest()


# --------------------------------------------------------------------------- #
# Curry / uncurry.
# --------------------------------------------------------------------------- #

def curry(program, *args) -> SExp:
    """Curry ``args`` into ``program``.

    Produces ``(a (q . program) ARGS)`` where ``ARGS`` is the nested
    ``(c (q . arg0) (c (q . arg1) ... 1))`` environment. Matches
    ``chia`` ``Program.curry`` byte-for-byte.
    """
    fixed = SExp.to(1)
    for arg in reversed(args):
        fixed = SExp.to([_OP_CONS, (_OP_QUOTE, arg), fixed])
    return SExp.to([_OP_APPLY, (_OP_QUOTE, program), fixed])


def uncurry(program) -> Optional[Tuple[SExp, list]]:
    """Inverse of :func:`curry`.

    Returns ``(module, [arg0, arg1, ...])`` or ``None`` if ``program`` is not in
    curried form. Used to parse the portal singleton's lineage (its parent's
    inner puzzle) when syncing -- the only place the bridge inspects existing
    on-chain puzzles rather than building new ones.
    """
    try:
        node = SExp.to(program)
        if node.pair is None or node.pair[0].atom != _OP_APPLY:
            return None
        rest = node.pair[1]
        quoted_mod = rest.pair[0]
        if quoted_mod.pair is None or quoted_mod.pair[0].atom != _OP_QUOTE:
            return None
        module = quoted_mod.pair[1]

        args: list = []
        arg_node = rest.pair[1].pair[0]
        while arg_node.pair is not None:  # terminates at the env atom (1)
            if arg_node.pair[0].atom != _OP_CONS:
                return None
            quoted_arg = arg_node.pair[1].pair[0]
            if quoted_arg.pair is None or quoted_arg.pair[0].atom != _OP_QUOTE:
                return None
            args.append(quoted_arg.pair[1])
            arg_node = arg_node.pair[1].pair[1].pair[0]
        return module, args
    except (AttributeError, TypeError, ValueError):
        return None


# --------------------------------------------------------------------------- #
# Integer encoding + coin ids.
# --------------------------------------------------------------------------- #

def int_to_bytes(value: int) -> bytes:
    """Canonical CLVM/chia integer encoding (minimal signed big-endian).

    ``0`` encodes to ``b""``; positive values that would set the sign bit gain a
    leading ``0x00``. This is exactly the encoding chia uses for coin amounts, so
    it feeds :func:`coin_name` directly.
    """
    if value == 0:
        return b""
    byte_count = (value.bit_length() + 8) >> 3
    return value.to_bytes(byte_count, "big", signed=True)


def coin_name(parent_coin_id: bytes, puzzle_hash: bytes, amount: int) -> bytes:
    """Coin id = sha256(parent_coin_id . puzzle_hash . int_to_bytes(amount))."""
    return hashlib.sha256(
        parent_coin_id + puzzle_hash + int_to_bytes(amount)
    ).digest()


# --------------------------------------------------------------------------- #
# Puzzle loading (hash-asserted).
# --------------------------------------------------------------------------- #

_puzzle_cache: dict[str, SExp] = {}


def load_puzzle(name: str) -> SExp:
    """Load a vendored puzzle reveal by name, asserting its tree hash.

    ``name`` is the file stem (e.g. ``"cat_v2"``). Raises ``KeyError`` if there
    is no expected hash on record and ``ValueError`` if the on-disk reveal does
    not match it -- the load-time correctness anchor that makes a tampered or
    wrong-version puzzle fail closed.
    """
    cached = _puzzle_cache.get(name)
    if cached is not None:
        return cached

    expected = EXPECTED_TREE_HASHES.get(name)
    if expected is None:
        raise KeyError(f"no expected tree hash registered for puzzle {name!r}")

    path = _PUZZLE_DIR / f"{name}.clsp.hex"
    program = program_from_hex(path.read_text())
    actual = sha256tree(program).hex()
    if actual != expected:
        raise ValueError(
            f"puzzle {name!r} tree hash {actual} != expected {expected}"
        )

    _puzzle_cache[name] = program
    return program


def puzzle_hash(name: str) -> bytes:
    """Tree hash of a vendored puzzle reveal (hash-asserted via load)."""
    return sha256tree(load_puzzle(name))


# --------------------------------------------------------------------------- #
# bech32m (BIP-350).
# --------------------------------------------------------------------------- #
#
# Chia addresses and warp's Nostr r/c/s tags are all bech32m (constant
# 0x2bc830a3). This mirrors chia.util.bech32m exactly -- warp's cli imports the
# same functions to build the Nostr filter tags, so byte-for-byte compatibility
# here is what lets us find the validator signature events.

_BECH32_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
_BECH32M_CONST = 0x2BC830A3


def _bech32_polymod(values: Sequence[int]) -> int:
    generator = [0x3B6A57B2, 0x26508E6D, 0x1EA119FA, 0x3D4233DD, 0x2A1462B3]
    chk = 1
    for value in values:
        top = chk >> 25
        chk = (chk & 0x1FFFFFF) << 5 ^ value
        for i in range(5):
            chk ^= generator[i] if ((top >> i) & 1) else 0
    return chk


def _bech32_hrp_expand(hrp: str) -> list[int]:
    return [ord(x) >> 5 for x in hrp] + [0] + [ord(x) & 31 for x in hrp]


def _bech32_verify_checksum(hrp: str, data: Sequence[int]) -> bool:
    return _bech32_polymod(_bech32_hrp_expand(hrp) + list(data)) == _BECH32M_CONST


def _bech32_create_checksum(hrp: str, data: Sequence[int]) -> list[int]:
    values = _bech32_hrp_expand(hrp) + list(data)
    polymod = _bech32_polymod(values + [0, 0, 0, 0, 0, 0]) ^ _BECH32M_CONST
    return [(polymod >> 5 * (5 - i)) & 31 for i in range(6)]


def bech32_encode(hrp: str, data: Sequence[int]) -> str:
    """Encode 5-bit ``data`` under ``hrp`` with a bech32m checksum."""
    combined = list(data) + _bech32_create_checksum(hrp, data)
    return hrp + "1" + "".join(_BECH32_CHARSET[d] for d in combined)


def bech32_decode(bech: str, max_length: int = 90) -> Tuple[Optional[str], Optional[list]]:
    """Decode a bech32m string to ``(hrp, 5-bit data)`` or ``(None, None)``."""
    if any(ord(x) < 33 or ord(x) > 126 for x in bech):
        return (None, None)
    if bech.lower() != bech and bech.upper() != bech:
        return (None, None)
    bech = bech.lower()
    pos = bech.rfind("1")
    if pos < 1 or pos + 7 > len(bech) or len(bech) > max_length:
        return (None, None)
    if not all(x in _BECH32_CHARSET for x in bech[pos + 1:]):
        return (None, None)
    hrp = bech[:pos]
    data = [_BECH32_CHARSET.find(x) for x in bech[pos + 1:]]
    if not _bech32_verify_checksum(hrp, data):
        return (None, None)
    return (hrp, data[:-6])


def convertbits(data: Iterable[int], frombits: int, tobits: int, pad: bool = True) -> list[int]:
    """General power-of-2 base conversion (BIP-173)."""
    acc = 0
    bits = 0
    ret: list[int] = []
    maxv = (1 << tobits) - 1
    max_acc = (1 << (frombits + tobits - 1)) - 1
    for value in data:
        if value < 0 or (value >> frombits):
            raise ValueError("invalid value for convertbits")
        acc = ((acc << frombits) | value) & max_acc
        bits += frombits
        while bits >= tobits:
            bits -= tobits
            ret.append((acc >> bits) & maxv)
    if pad:
        if bits:
            ret.append((acc << (tobits - bits)) & maxv)
    elif bits >= frombits or ((acc << (tobits - bits)) & maxv):
        raise ValueError("invalid padding in convertbits")
    return ret


def bech32m_encode_bytes(hrp: str, data: bytes) -> str:
    """Encode raw ``data`` bytes as a bech32m string (8->5 bit repack)."""
    return bech32_encode(hrp, convertbits(data, 8, 5))


def bech32m_decode_bytes(bech: str, max_length: int = 90) -> Tuple[Optional[str], Optional[bytes]]:
    """Decode a bech32m string to ``(hrp, raw bytes)`` or ``(None, None)``."""
    hrp, data = bech32_decode(bech, max_length)
    if data is None:
        return (None, None)
    return (hrp, bytes(convertbits(data, 5, 8, False)))


def encode_puzzle_hash(puzzle_hash_bytes: bytes, prefix: str) -> str:
    """Encode a 32-byte puzzle hash as an address (e.g. ``xch1...``)."""
    return bech32m_encode_bytes(prefix, puzzle_hash_bytes)


def decode_puzzle_hash(address: str, expected_prefix: Optional[str] = None) -> bytes:
    """Decode an ``xch``/``txch`` address to its 32-byte puzzle hash."""
    hrp, decoded = bech32m_decode_bytes(address)
    if decoded is None:
        raise ValueError(f"invalid bech32m address: {address!r}")
    if expected_prefix is not None and hrp != expected_prefix:
        raise ValueError(
            f"address prefix {hrp!r} != expected {expected_prefix!r}"
        )
    return decoded
