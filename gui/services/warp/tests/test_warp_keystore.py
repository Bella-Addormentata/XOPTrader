"""Tests for the warp keystore.

The envelope logic (base64, entropy binding, EVM/BLS key round-trips) is tested
on every platform with an injected fake :class:`SecretProtector`. The real DPAPI
path is exercised by Windows-only tests that use the default protector.
"""

from __future__ import annotations

import hashlib
import os
import sys

import pytest

# eth_account (EVM keys) and chia_rs (BLS keys) are required by the keystore.
pytest.importorskip("eth_account")
pytest.importorskip("chia_rs")

from gui.services.warp import keystore  # noqa: E402
from gui.services.warp.keystore import (  # noqa: E402
    BlsKey,
    EvmKey,
    KeystoreError,
    KeystoreUnavailable,
)


class FakeProtector:
    """Stateless, reversible stand-in for DPAPI that emulates entropy binding.

    Not encryption -- it only needs to round-trip and to *detect* a wrong
    entropy value the way ``CryptUnprotectData`` does, so the envelope code can
    be tested off-Windows.
    """

    def protect(self, data: bytes, entropy: bytes = b"") -> bytes:
        tag = hashlib.sha256(entropy).digest()[:8]
        return b"FAKE1" + tag + bytes(data)

    def unprotect(self, blob: bytes, entropy: bytes = b"") -> bytes:
        if not blob.startswith(b"FAKE1"):
            raise KeystoreError("not a fake protector blob")
        tag, data = blob[5:13], blob[13:]
        if tag != hashlib.sha256(entropy).digest()[:8]:
            raise KeystoreError("entropy mismatch")
        return data


# --------------------------------------------------------------------------- #
# EVM keys.
# --------------------------------------------------------------------------- #

def test_new_evm_key_shape():
    k = keystore.new_evm_key()
    assert isinstance(k, EvmKey)
    assert len(k.private_key) == 32
    assert k.address.startswith("0x") and len(k.address) == 42


def test_evm_key_import_roundtrip_prefixed_and_bare():
    k = keystore.new_evm_key()
    bare = keystore.evm_key_from_hex(k.private_key.hex())
    prefixed = keystore.evm_key_from_hex("0x" + k.private_key.hex())
    assert bare.address == k.address == prefixed.address
    assert bare.private_key == k.private_key == prefixed.private_key


def test_evm_key_import_rejects_bad_hex_and_length():
    with pytest.raises(KeystoreError):
        keystore.evm_key_from_hex("nothex!!")
    with pytest.raises(KeystoreError):
        keystore.evm_key_from_hex("00" * 31)  # 31 bytes
    with pytest.raises(KeystoreError):
        EvmKey(b"\x00" * 31, "0x" + "00" * 20)


def test_evm_key_protect_load_with_fake():
    fake = FakeProtector()
    k = keystore.new_evm_key()
    blob = keystore.protect_evm_key(k, protector=fake)
    assert isinstance(blob, str)  # base64, YAML-safe
    loaded = keystore.load_evm_key(blob, protector=fake)
    assert loaded.address == k.address
    assert loaded.private_key == k.private_key


# --------------------------------------------------------------------------- #
# BLS keys.
# --------------------------------------------------------------------------- #

def test_new_bls_key_shape_and_reload():
    k = keystore.new_bls_key()
    assert isinstance(k, BlsKey)
    assert len(k.private_key) == 32 and len(k.public_key) == 48
    reloaded = keystore.bls_key_from_bytes(k.private_key)
    assert reloaded.public_key == k.public_key  # deterministic G1 from priv


def test_bls_key_rejects_bad_length():
    with pytest.raises(KeystoreError):
        keystore.bls_key_from_bytes(b"\x00" * 16)
    with pytest.raises(KeystoreError):
        BlsKey(b"\x00" * 16, b"\x00" * 48)


def test_bls_key_protect_load_with_fake_and_entropy_binding():
    fake = FakeProtector()
    k = keystore.new_bls_key()
    blob = keystore.protect_bls_key(k, extra_entropy=b"job-1", protector=fake)
    # Correct entropy round-trips.
    loaded = keystore.load_bls_key(blob, extra_entropy=b"job-1", protector=fake)
    assert loaded.private_key == k.private_key
    assert loaded.public_key == k.public_key
    # Wrong per-job entropy is rejected (emulating DPAPI account/entropy binding).
    with pytest.raises(KeystoreError):
        keystore.load_bls_key(blob, extra_entropy=b"job-2", protector=fake)


# --------------------------------------------------------------------------- #
# Envelope edge cases + platform gating.
# --------------------------------------------------------------------------- #

def test_unprotect_rejects_non_base64():
    fake = FakeProtector()
    with pytest.raises(KeystoreError):
        keystore.unprotect_secret("not base64 @@@", protector=fake)


def test_secure_store_availability_by_platform():
    assert keystore.is_secure_store_available("win32") is True
    assert keystore.is_secure_store_available("linux") is False
    assert keystore.is_secure_store_available("darwin") is False


def test_default_protector_refuses_non_windows():
    with pytest.raises(KeystoreUnavailable):
        keystore.default_protector(platform="linux")


# --------------------------------------------------------------------------- #
# Real DPAPI (Windows only).
# --------------------------------------------------------------------------- #

@pytest.mark.skipif(sys.platform != "win32", reason="DPAPI is Windows-only")
def test_dpapi_roundtrip_and_entropy_binding():
    p = keystore.DpapiProtector()
    secret = os.urandom(32)
    blob = p.protect(secret, b"entropy-A")
    assert blob != secret
    assert p.unprotect(blob, b"entropy-A") == secret
    with pytest.raises(KeystoreError):
        p.unprotect(blob, b"entropy-B")


@pytest.mark.skipif(sys.platform != "win32", reason="DPAPI is Windows-only")
def test_evm_key_store_load_with_real_dpapi():
    k = keystore.new_evm_key()
    blob = keystore.protect_evm_key(k)  # default (DPAPI) protector
    loaded = keystore.load_evm_key(blob)
    assert loaded.address == k.address
    assert loaded.private_key == k.private_key
