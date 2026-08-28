"""The Permuto trading identity: a persistent BLS key, recoverable from words.

This is the first PERSISTENT Chia secret the project has held. Warp's BLS keys
are deliberately ephemeral, one per job, and losing one costs a retry. This key
*is* the exchange account: lose it and the entry, the standing and (once
Permuto moves off simulated capital) the balance go with it. The design follows
from that single fact.

WHY A MNEMONIC, WHEN THE BASE WALLET USES RAW HEX.  ``docs/base-wallet-key-
backup-options.md`` argues against BIP-39 words for the EVM key, and it is
right to: rendering a raw secp256k1 key as 24 words is *actively misleading*,
because any standard wallet treats those words as a SEED and derives a
different address from them. Someone recovering under stress would import the
words, see an unrelated empty account, and conclude the funds were gone.

Chia inverts that argument, because for Chia the words ARE the standard:

    24 words --BIP-39 PBKDF2--> 64-byte seed --AugSchemeMPL.key_gen--> master SK

That is Chia's own derivation, not a re-encoding around it. So the same 24
words restore this identity in any Chia wallet, and the trap the Base document
warns about does not exist here. We generate FROM the mnemonic rather than
rendering one afterwards, which is what makes the guarantee real.

Two layers, deliberately not alternatives:

* **The mnemonic** protects against losing the machine. Paper or metal, off
  this box, written down once at creation.
* **DPAPI (or a passphrase off Windows)** protects the copy at rest, so a
  stolen ``secrets.yaml`` is not a stolen account.

The wordlist ships inside ``eth_account``, already a declared dependency, so
this adds no package and downloads nothing. Its BIP-39 implementation is
checked against the canonical vectors in the tests rather than trusted.
"""

from __future__ import annotations

import logging
import secrets as _secrets
from contextlib import nullcontext
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Optional

_log = logging.getLogger(__name__)

#: Where the identity lives inside ``secrets.yaml``.
_SECTION = "permuto"

#: BIP-39 entropy in bytes. 32 -> 24 words, matching Chia's own backup length
#: so an operator who has done this before recognises the artifact.
_ENTROPY_BYTES = 32

#: Bound into the at-rest wrapper so a Permuto blob cannot be unwrapped as a
#: warp blob and vice versa, even on the same machine and account.
_IDENTITY_ENTROPY = b"XOPTrader/permuto/identity/v1"


class PermutoIdentityError(RuntimeError):
    """Operator-facing failure creating, loading or restoring the identity."""


def _io_transaction(secrets_io: Any):
    """The secrets file's read-modify-write lock, or a no-op for fakes."""
    txn = getattr(secrets_io, "transaction", None)
    return txn() if callable(txn) else nullcontext()


# --------------------------------------------------------------------------- #
# BIP-39, via the wordlist eth_account already ships.
# --------------------------------------------------------------------------- #

#: We always generate 24 words. A *valid* 12-word phrase is still BIP-39
#: legal and derives a perfectly good -- and completely different -- key, so
#: accepting one would hand the operator a working, empty account with no
#: error anywhere. Length is checked as well as checksum.
_WORD_COUNT = 24


def _mnemonic_api():
    from eth_account.hdaccount.mnemonic import Language, Mnemonic

    return Mnemonic(Language.ENGLISH)


def generate_mnemonic() -> str:
    """A fresh 24-word BIP-39 phrase from the OS CSPRNG.

    ``secrets.token_bytes`` rather than ``random``: this is the only entropy
    that will ever protect the account, and it is generated exactly once.
    """
    return _mnemonic_api().to_mnemonic(_secrets.token_bytes(_ENTROPY_BYTES))


def mnemonic_is_valid(phrase: str) -> bool:
    """Checksum-check a phrase before we act on it.

    BIP-39's checksum is the whole point of using words: it catches the
    transcription errors that raw hex cannot. Restore must refuse a phrase
    that fails it rather than silently deriving some other account.
    """
    phrase = phrase.strip()
    if len(phrase.split()) != _WORD_COUNT:
        return False
    try:
        return bool(_mnemonic_api().is_mnemonic_valid(phrase))
    except Exception:  # noqa: BLE001 - malformed input is simply invalid
        return False


def derive_bls_key(phrase: str):
    """``phrase`` -> Chia master BLS key, by Chia's own derivation.

    Returns the ``chia_rs`` PrivateKey. Kept as a free function so the exact
    derivation can be tested against Chia's without constructing an identity
    or touching any file.
    """
    from eth_account.hdaccount.mnemonic import Mnemonic
    import chia_rs

    phrase = phrase.strip()
    words = len(phrase.split())
    if words != _WORD_COUNT:
        raise PermutoIdentityError(
            "recovery phrase must be %d words, got %d. A shorter BIP-39 "
            "phrase is still valid and would derive a DIFFERENT, empty "
            "account rather than failing -- refusing." % (_WORD_COUNT, words)
        )
    if not mnemonic_is_valid(phrase):
        raise PermutoIdentityError(
            "recovery phrase failed its BIP-39 checksum -- check for a "
            "mistyped or transposed word; no key was derived"
        )
    # Empty passphrase is Chia's convention. A non-empty one would silently
    # produce a DIFFERENT key from the same words, which is precisely the
    # class of surprise this module exists to avoid.
    seed = Mnemonic.to_seed(phrase, "")
    return chia_rs.AugSchemeMPL.key_gen(seed)


@dataclass(frozen=True)
class IdentityInfo:
    """Public facts about the identity. Contains no secret material."""

    pubkey: str
    """48-byte BLS G1 public key, hex. What Permuto knows us by."""

    created_at: Optional[str]
    backup_confirmed: bool
    registered: bool
    user_id: Optional[str] = None
    trading_address: Optional[str] = None
    listing_verified: bool = False
    """The venue's leaderboard has actually listed us.

    Distinct from ``registered``, which only means the link call succeeded.
    Without this the page would render green on any later refresh, because
    the durable state cannot otherwise tell a confirmed listing from an
    unconfirmed one -- and green is the whole point of the check.
    """


class PermutoIdentity:
    """Create, load and back up the persistent Permuto signing identity.

    ``secrets_io`` is the same read/write/transaction shape the Base wallet
    uses; ``protector`` is the keystore's :class:`SecretProtector` (DPAPI in
    production, fakes in tests).
    """

    def __init__(self, secrets_io: Any, *, protector: Any = None) -> None:
        self._io = secrets_io
        self._protector = protector

    def _wrapper(self):
        """The at-rest protector, resolved on first ACTUAL use.

        Resolving eagerly meant constructing an identity raised on every
        non-Windows platform, which took the whole page down with it -- even
        though reading the public key, the address and the registration state
        needs no protector at all. Deferring it keeps inspection working
        everywhere and puts the platform error on the operation that genuinely
        requires a secure store.
        """
        if self._protector is None:
            from ..warp.keystore import default_protector

            self._protector = default_protector()
        return self._protector

    # -- internals ---------------------------------------------------------- #

    def _section(self, secrets: dict) -> dict:
        return secrets.setdefault(_SECTION, {})

    def exists(self) -> bool:
        section = self._io.read().get(_SECTION) or {}
        return bool(section.get("bls_private_key_dpapi"))

    # -- creation ----------------------------------------------------------- #

    def create(self) -> tuple[str, str]:
        """Generate the identity. Returns ``(pubkey_hex, mnemonic)``.

        The mnemonic is returned ONCE and never stored. Showing it again later
        would mean keeping it at rest, which defeats the point of holding only
        a DPAPI-wrapped key. The caller must display it, get confirmation, and
        drop it.

        Refuses to overwrite an existing identity: on this venue that is not a
        re-roll, it is abandoning an account that may hold standing or funds.
        """
        from ..warp import keystore

        with _io_transaction(self._io):
            secrets = self._io.read()
            if (secrets.get(_SECTION) or {}).get("bls_private_key_dpapi"):
                raise PermutoIdentityError(
                    "a Permuto identity already exists; creating another "
                    "would abandon it. Restore from the recovery phrase, or "
                    "remove the existing identity deliberately."
                )

            phrase = generate_mnemonic()
            sk = derive_bls_key(phrase)
            pubkey = bytes(sk.get_g1()).hex()

            section = self._section(secrets)
            section["bls_private_key_dpapi"] = keystore.protect_secret(
                bytes(sk),
                extra_entropy=_IDENTITY_ENTROPY,
                protector=self._wrapper(),
            )
            section["bls_public_key"] = pubkey
            section["created_at"] = datetime.now(timezone.utc).isoformat()
            section["backup_confirmed"] = False
            section["registered"] = False
            self._io.write(secrets)

        _log.info("permuto: identity created, pubkey %s...", pubkey[:16])
        return pubkey, phrase

    def restore(self, phrase: str) -> str:
        """Re-derive the identity from its recovery phrase. Returns pubkey hex.

        Overwrites whatever is stored, because the caller is asserting these
        words are the account. The checksum in :func:`derive_bls_key` is what
        stops a typo installing a stranger's key here.
        """
        from ..warp import keystore

        sk = derive_bls_key(phrase)
        pubkey = bytes(sk.get_g1()).hex()

        with _io_transaction(self._io):
            secrets = self._io.read()
            section = self._section(secrets)
            # Restoring a DIFFERENT phrase installs a different account.
            # Registration metadata describes the old one: left in place it
            # would disable the Register button and display someone else's
            # user_id and address against the new key.
            if section.get("bls_public_key") not in (None, pubkey):
                for stale in ("registered", "user_id", "trading_address",
                              "registered_at", "listing_verified"):
                    section.pop(stale, None)
            section["bls_private_key_dpapi"] = keystore.protect_secret(
                bytes(sk),
                extra_entropy=_IDENTITY_ENTROPY,
                protector=self._wrapper(),
            )
            section["bls_public_key"] = pubkey
            section.setdefault("created_at",
                               datetime.now(timezone.utc).isoformat())
            # Restoring proves the phrase is written down somewhere.
            section["backup_confirmed"] = True
            self._io.write(secrets)

        _log.info("permuto: identity restored, pubkey %s...", pubkey[:16])
        return pubkey

    # -- use ---------------------------------------------------------------- #

    def private_key(self):
        """The live signing key. Never log, never serialise, never display."""
        from ..warp import keystore
        import chia_rs

        section = self._io.read().get(_SECTION) or {}
        blob = section.get("bls_private_key_dpapi")
        if not blob:
            raise PermutoIdentityError(
                "no Permuto identity on this machine -- create one, or "
                "restore it from its 24-word recovery phrase"
            )
        raw = keystore.unprotect_secret(
            blob, extra_entropy=_IDENTITY_ENTROPY, protector=self._wrapper()
        )
        return chia_rs.PrivateKey.from_bytes(raw)

    def public_key(self) -> str:
        section = self._io.read().get(_SECTION) or {}
        pubkey = section.get("bls_public_key")
        if not pubkey:
            raise PermutoIdentityError("no Permuto identity on this machine")
        return str(pubkey)

    def sign(self, message: bytes) -> bytes:
        """Raw AugSchemeMPL signature over ``message``.

        Permuto's link challenge is signed over the 32 decoded nonce bytes
        directly -- NOT CHIP-0002's "Chia Signed Message" envelope, which is
        what a WalletConnect wallet would produce. The two are not
        interchangeable and a CHIP-0002 signature will not verify here.
        """
        import chia_rs

        return bytes(chia_rs.AugSchemeMPL.sign(self.private_key(), message))

    # -- backup bookkeeping ------------------------------------------------- #

    def mark_backup_confirmed(self) -> None:
        with _io_transaction(self._io):
            secrets = self._io.read()
            self._section(secrets)["backup_confirmed"] = True
            self._io.write(secrets)

    def mark_registered(
        self, *, user_id: str, trading_address: str,
        listing_verified: bool = False,
    ) -> None:
        """Record the link. ``listing_verified`` only when the board shows us.

        Never downgrades a verified listing: a later check that happens to
        run while the board is rebuilding must not retract a confirmation we
        already earned.
        """
        with _io_transaction(self._io):
            secrets = self._io.read()
            section = self._section(secrets)
            section["registered"] = True
            section["user_id"] = user_id
            section["trading_address"] = trading_address
            section.setdefault(
                "registered_at", datetime.now(timezone.utc).isoformat()
            )
            if listing_verified:
                section["listing_verified"] = True
            else:
                section.setdefault("listing_verified", False)
            self._io.write(secrets)

    def info(self) -> IdentityInfo:
        section = self._io.read().get(_SECTION) or {}
        if not section.get("bls_public_key"):
            raise PermutoIdentityError("no Permuto identity on this machine")
        return IdentityInfo(
            pubkey=str(section["bls_public_key"]),
            created_at=section.get("created_at"),
            backup_confirmed=bool(section.get("backup_confirmed")),
            registered=bool(section.get("registered")),
            user_id=section.get("user_id"),
            trading_address=section.get("trading_address"),
            listing_verified=bool(section.get("listing_verified")),
        )
