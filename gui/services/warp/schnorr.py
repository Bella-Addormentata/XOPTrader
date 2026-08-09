"""Pure-Python BIP340 Schnorr signatures over secp256k1.

Nostr events (NIP-01) are signed with BIP340, and no Schnorr-capable
secp256k1 library ships with this environment -- so, as with the EIP-712 and
bech32m layers, the primitive is implemented here and pinned against the
official BIP340 test vectors (``tests/fixtures_bip340_vectors.csv``,
downloaded verbatim from the bitcoin/bips repository).

Scope: exactly what the altruistic-relay heartbeat needs -- key generation,
signing, verification. Performance is irrelevant at one signature per ten
minutes, so the arithmetic is plain affine coordinates with Fermat inverses,
chosen for auditability over speed. Not constant-time; the key it signs with
is a throwaway identity derived for heartbeats, never a funds key.
"""

from __future__ import annotations

import hashlib
from typing import Optional, Tuple

# secp256k1 domain parameters.
P = 2 ** 256 - 2 ** 32 - 977
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (
    0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
    0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8,
)

Point = Optional[Tuple[int, int]]  # None is the point at infinity


class SchnorrError(ValueError):
    """A key or signature input was structurally invalid."""


def tagged_hash(tag: str, msg: bytes) -> bytes:
    """BIP340 tagged hash: sha256(sha256(tag) || sha256(tag) || msg)."""
    tag_hash = hashlib.sha256(tag.encode()).digest()
    return hashlib.sha256(tag_hash + tag_hash + msg).digest()


# --------------------------------------------------------------------------- #
# Point arithmetic (affine; clarity over speed).
# --------------------------------------------------------------------------- #

def _point_add(p1: Point, p2: Point) -> Point:
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if p1 == p2:
        lam = (3 * x1 * x1) * pow(2 * y1, P - 2, P) % P
    else:
        lam = (y2 - y1) * pow(x2 - x1, P - 2, P) % P
    x3 = (lam * lam - x1 - x2) % P
    return (x3, (lam * (x1 - x3) - y1) % P)


def _point_mul(point: Point, scalar: int) -> Point:
    result: Point = None
    addend = point
    while scalar:
        if scalar & 1:
            result = _point_add(result, addend)
        addend = _point_add(addend, addend)
        scalar >>= 1
    return result


def _has_even_y(point: Point) -> bool:
    return point is not None and point[1] % 2 == 0


def _bytes_x(point: Point) -> bytes:
    assert point is not None
    return point[0].to_bytes(32, "big")


def lift_x(x: int) -> Point:
    """The point with x-coordinate ``x`` and even y, or ``None`` if not on curve."""
    if x >= P:
        return None
    y_sq = (pow(x, 3, P) + 7) % P
    y = pow(y_sq, (P + 1) // 4, P)
    if pow(y, 2, P) != y_sq:
        return None
    return (x, y if y % 2 == 0 else P - y)


# --------------------------------------------------------------------------- #
# The BIP340 API.
# --------------------------------------------------------------------------- #

def pubkey_gen(seckey: bytes) -> bytes:
    """The 32-byte x-only public key for a 32-byte secret key."""
    d0 = int.from_bytes(seckey, "big")
    if not 1 <= d0 <= N - 1:
        raise SchnorrError("secret key is out of range")
    return _bytes_x(_point_mul(G, d0))


def sign(msg: bytes, seckey: bytes, aux_rand: bytes) -> bytes:
    """BIP340 signature over ``msg`` (any length) with 32 bytes of aux randomness.

    Follows the reference algorithm exactly, including the self-verify at the
    end -- a corrupted signature must never leave this function.
    """
    d0 = int.from_bytes(seckey, "big")
    if not 1 <= d0 <= N - 1:
        raise SchnorrError("secret key is out of range")
    if len(aux_rand) != 32:
        raise SchnorrError("aux_rand must be exactly 32 bytes")
    point = _point_mul(G, d0)
    d = d0 if _has_even_y(point) else N - d0
    t = (d ^ int.from_bytes(tagged_hash("BIP0340/aux", aux_rand), "big")).to_bytes(32, "big")
    k0 = int.from_bytes(
        tagged_hash("BIP0340/nonce", t + _bytes_x(point) + msg), "big"
    ) % N
    if k0 == 0:
        raise SchnorrError("nonce derivation failed (k = 0)")
    r_point = _point_mul(G, k0)
    k = k0 if _has_even_y(r_point) else N - k0
    e = int.from_bytes(
        tagged_hash("BIP0340/challenge", _bytes_x(r_point) + _bytes_x(point) + msg),
        "big",
    ) % N
    sig = _bytes_x(r_point) + ((k + e * d) % N).to_bytes(32, "big")
    if not verify(msg, _bytes_x(point), sig):
        raise SchnorrError("self-verification of the fresh signature failed")
    return sig


def verify(msg: bytes, pubkey: bytes, sig: bytes) -> bool:
    """BIP340 verification; malformed inputs are ``False``, never an exception."""
    if len(pubkey) != 32 or len(sig) != 64:
        return False
    point = lift_x(int.from_bytes(pubkey, "big"))
    if point is None:
        return False
    r = int.from_bytes(sig[0:32], "big")
    s = int.from_bytes(sig[32:64], "big")
    if r >= P or s >= N:
        return False
    e = int.from_bytes(
        tagged_hash("BIP0340/challenge", sig[0:32] + pubkey + msg), "big"
    ) % N
    r_point = _point_add(_point_mul(G, s), _point_mul(point, N - e))
    if r_point is None or not _has_even_y(r_point) or r_point[0] != r:
        return False
    return True
