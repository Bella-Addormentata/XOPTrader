"""Identity tests. The derivation ones are the load-bearing part.

If the mnemonic-to-key derivation is wrong in any way, the 24 words we tell an
operator to engrave on metal will not restore the account -- and nothing will
reveal that until the day they need it. So the canonical BIP-39 vectors are
pinned here rather than assumed, and the round-trip is exercised end to end.
"""

from __future__ import annotations

import pytest

from gui.services.permuto.identity import (
    PermutoIdentity,
    PermutoIdentityError,
    derive_bls_key,
    generate_mnemonic,
    mnemonic_is_valid,
)


# --------------------------------------------------------------------------- #
# Fakes
# --------------------------------------------------------------------------- #

class FakeSecretsIO:
    def __init__(self, data=None):
        self._data = data or {}

    def read(self):
        import copy

        return copy.deepcopy(self._data)

    def write(self, data):
        import copy

        self._data = copy.deepcopy(data)


class FakeProtector:
    """Reversible, not secret. Proves the wiring without needing Windows."""

    def protect(self, data: bytes, entropy: bytes = b"") -> bytes:
        return b"P|" + entropy + b"|" + data

    def unprotect(self, blob: bytes, entropy: bytes = b"") -> bytes:
        prefix = b"P|" + entropy + b"|"
        if not blob.startswith(prefix):
            raise ValueError("wrong entropy or corrupt blob")
        return blob[len(prefix):]


@pytest.fixture()
def ident():
    return PermutoIdentity(FakeSecretsIO(), protector=FakeProtector())


# --------------------------------------------------------------------------- #
# BIP-39 correctness -- canonical vectors, not our own output
# --------------------------------------------------------------------------- #

def test_bip39_canonical_24_word_vector():
    """256 bits of zero entropy is 'abandon' x23 + 'art'.

    Pins wordlist content AND ordering. A shuffled or truncated list would
    still produce 24 plausible words, and only this catches it.
    """
    from eth_account.hdaccount.mnemonic import Mnemonic

    assert Mnemonic("english").to_mnemonic(bytes(32)) == \
        "abandon " * 23 + "art"


def test_bip39_canonical_seed_vector():
    """The published TREZOR-passphrase seed vector.

    Pins the PBKDF2 parameters -- 2048 rounds, HMAC-SHA512, salt
    'mnemonic'+passphrase. Get any of those wrong and every derived key is
    wrong while still looking perfectly well-formed.
    """
    from eth_account.hdaccount.mnemonic import Mnemonic

    seed = Mnemonic.to_seed("abandon " * 11 + "about", "TREZOR")
    assert seed.hex().startswith("c55257c360c07c72029aebc1b53c05ed")
    assert len(seed) == 64


def test_derivation_is_deterministic_and_chia_shaped():
    phrase = "abandon " * 23 + "art"
    a, b = derive_bls_key(phrase), derive_bls_key(phrase)
    assert bytes(a) == bytes(b)
    assert len(bytes(a.get_g1())) == 48        # G1 public key
    assert len(bytes(a)) == 32                 # master secret


def test_derivation_uses_an_empty_passphrase():
    """Chia's convention. A non-empty passphrase silently yields a DIFFERENT
    key from the same words -- exactly the surprise that loses an account."""
    from eth_account.hdaccount.mnemonic import Mnemonic
    import chia_rs

    phrase = "abandon " * 23 + "art"
    expected = chia_rs.AugSchemeMPL.key_gen(Mnemonic.to_seed(phrase, ""))
    assert bytes(derive_bls_key(phrase)) == bytes(expected)


def test_generated_mnemonics_are_24_words_and_valid():
    phrase = generate_mnemonic()
    assert len(phrase.split()) == 24
    assert mnemonic_is_valid(phrase)


def test_generated_mnemonics_are_unique():
    """Cheap guard against a seeded or reused RNG."""
    assert len({generate_mnemonic() for _ in range(5)}) == 5


# --------------------------------------------------------------------------- #
# The checksum is the reason we chose words
# --------------------------------------------------------------------------- #

def test_a_transposed_word_is_refused():
    good = "abandon " * 23 + "art"
    bad = " ".join((good.split()[:22] + ["art", "abandon"]))
    assert not mnemonic_is_valid(bad)
    with pytest.raises(PermutoIdentityError, match="checksum"):
        derive_bls_key(bad)


def test_a_misspelled_word_is_refused():
    bad = ("abandon " * 23 + "artt")
    assert not mnemonic_is_valid(bad)
    with pytest.raises(PermutoIdentityError):
        derive_bls_key(bad)


def test_wrong_length_is_refused():
    with pytest.raises(PermutoIdentityError):
        derive_bls_key("abandon " * 11 + "about")  # valid 12-word, wrong size


# --------------------------------------------------------------------------- #
# Storage round-trip
# --------------------------------------------------------------------------- #

def test_create_then_restore_reproduces_the_same_key(ident):
    """The whole promise of the mnemonic, exercised end to end."""
    pubkey, phrase = ident.create()

    fresh = PermutoIdentity(FakeSecretsIO(), protector=FakeProtector())
    assert fresh.restore(phrase) == pubkey


def test_create_refuses_to_overwrite_an_existing_identity(ident):
    ident.create()
    with pytest.raises(PermutoIdentityError, match="already exists"):
        ident.create()


def test_the_mnemonic_is_never_persisted(ident):
    """It is shown once and dropped. Storing it would defeat wrapping the key."""
    _, phrase = ident.create()
    blob = repr(ident._io.read())
    assert phrase not in blob
    assert "mnemonic" not in blob.lower()
    # Any 4 consecutive words would be enough to brute-force the rest.
    words = phrase.split()
    for i in range(len(words) - 3):
        assert " ".join(words[i:i + 4]) not in blob


def test_stored_key_is_wrapped_not_plaintext(ident):
    ident.create()
    section = ident._io.read()["permuto"]
    raw = bytes(ident.private_key())
    stored = repr(section)
    assert raw.hex() not in stored
    # base64 of the raw key must not appear either -- wrapping, not encoding.
    import base64
    assert base64.b64encode(raw).decode() not in stored


def test_signing_matches_the_public_key(ident):
    """A raw AugSchemeMPL signature over 32 bytes -- the shape wallet_auth
    verifies. NOT CHIP-0002, which a WalletConnect wallet would produce."""
    import chia_rs

    pubkey, _ = ident.create()
    nonce = bytes(range(32))
    sig = ident.sign(nonce)

    assert len(sig) == 96
    assert chia_rs.AugSchemeMPL.verify(
        chia_rs.G1Element.from_bytes(bytes.fromhex(pubkey)),
        nonce,
        chia_rs.G2Element.from_bytes(sig),
    )


def test_operations_without_an_identity_fail_loudly(ident):
    with pytest.raises(PermutoIdentityError, match="no Permuto identity"):
        ident.public_key()
    with pytest.raises(PermutoIdentityError, match="no Permuto identity"):
        ident.private_key()


def test_restore_marks_backup_confirmed(ident):
    """Restoring proves the words exist somewhere off this machine."""
    _, phrase = ident.create()
    assert ident.info().backup_confirmed is False
    fresh = PermutoIdentity(FakeSecretsIO(), protector=FakeProtector())
    fresh.restore(phrase)
    assert fresh.info().backup_confirmed is True


def test_identity_blob_is_bound_to_its_own_entropy(ident):
    """A warp blob must not unwrap as a Permuto blob on the same machine."""
    ident.create()
    section = ident._io.read()["permuto"]
    with pytest.raises(Exception):
        FakeProtector().unprotect(
            __import__("base64").b64decode(section["bls_private_key_dpapi"]),
            b"XOPTrader/warp/keystore/v1",
        )


# --------------------------------------------------------------------------- #
# Platform reach -- the page must not vanish where DPAPI does not exist
# --------------------------------------------------------------------------- #

def test_inspection_needs_no_protector_at_all():
    """default_protector() raises off Windows, and resolving it eagerly took
    the whole Permuto page down on Linux and macOS -- for a project that is
    open source and used worldwide. Reading PUBLIC state must not need one."""
    io_ = FakeSecretsIO({
        "permuto": {
            "bls_public_key": "ab" * 48,
            "bls_private_key_dpapi": "irrelevant",
            "registered": True,
            "user_id": "c" * 64,
            "trading_address": "xch1x",
            "listing_verified": True,
        }
    })
    ident = PermutoIdentity(io_)          # no protector supplied
    info = ident.info()                   # must not raise
    assert info.pubkey == "ab" * 48
    assert info.registered and info.listing_verified
    assert ident.public_key() == "ab" * 48
    assert ident.exists() is True


def test_the_protector_is_resolved_only_when_key_material_is_touched():
    """And when it IS needed, the platform error arrives attached to the
    operation that requires it rather than to page construction."""
    calls = []

    class Boom:
        def protect(self, data, entropy=b""):
            calls.append("protect")
            raise RuntimeError("no secure store on this platform")

        def unprotect(self, blob, entropy=b""):
            raise RuntimeError("no secure store on this platform")

    ident = PermutoIdentity(FakeSecretsIO(), protector=Boom())
    assert calls == []                    # construction touched nothing
    with pytest.raises(RuntimeError, match="secure store"):
        ident.create()
    assert calls == ["protect"]
