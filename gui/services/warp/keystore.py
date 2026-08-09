"""DPAPI-backed keystore for the warp bridge hot wallet and ephemeral keys.

The warp bridge holds two kinds of secret at rest:

* the **EVM hot-wallet private key** -- the secp256k1 key controlling the Base
  address the operator funds with USDC (and a little ETH for gas); and
* short-lived **ephemeral BLS keys** -- one per bridge job, used only to spend
  the Chia claim's security coin, retained until that coin is provably spent.

Both must survive a GUI restart on disk but never as plaintext. On Windows we
wrap them with DPAPI (``CryptProtectData``, ``CURRENT_USER`` scope) plus an
application-specific entropy blob, so the ciphertext is bound to the Windows
user account: a stolen ``secrets.yaml`` row (or ``warp_jobs.db`` row) is useless
on another machine or under another user. The stored form is base64 of the DPAPI
blob; the EVM key lives under ``warp.evm_private_key_dpapi`` in ``secrets.yaml``
(see ``SECRET_KEYS`` in :mod:`gui.services.config_split`).

Non-Windows platforms have no supported secure store here, so
:func:`default_protector` raises :class:`KeystoreUnavailable` rather than
silently persisting plaintext. Unit tests inject a fake :class:`SecretProtector`
so they run on any platform; the real DPAPI path is exercised by a
Windows-only test.

All heavy third-party imports (``eth_account``, ``chia_rs``) are done lazily
inside the functions that need them, so importing this module -- e.g. from the
GUI widget at construction time -- pulls in nothing but the standard library.
"""

from __future__ import annotations

import base64
import binascii
import os
import sys
from dataclasses import dataclass
from typing import Optional, Protocol

# Application-specific DPAPI entropy. Versioned so a future format change can be
# distinguished; changing it invalidates every previously stored blob.
_APP_ENTROPY = b"XOPTrader/warp/keystore/v1"

# CryptProtectData flag: never show a UI prompt (we run headless in a worker).
_CRYPTPROTECT_UI_FORBIDDEN = 0x1


class KeystoreError(RuntimeError):
    """A key could not be generated, imported, protected, or recovered."""


class KeystoreUnavailable(KeystoreError):
    """No supported secure store exists on this platform (non-Windows)."""


# --------------------------------------------------------------------------- #
# Secret protector abstraction (DPAPI in production, fakes in tests).
# --------------------------------------------------------------------------- #

class SecretProtector(Protocol):
    """Reversible, account-bound wrapper for raw secret bytes."""

    def protect(self, data: bytes, entropy: bytes = b"") -> bytes: ...

    def unprotect(self, blob: bytes, entropy: bytes = b"") -> bytes: ...


class DpapiProtector:
    """Windows DPAPI secret protector (``CryptProtectData``, current user).

    Constructed lazily; instantiating on a non-Windows platform raises
    :class:`KeystoreUnavailable`. The ``entropy`` argument is passed straight
    through to DPAPI as the optional entropy blob, so protect/unprotect must be
    called with identical entropy.
    """

    def __init__(self) -> None:
        if sys.platform != "win32":
            raise KeystoreUnavailable("DPAPI is only available on Windows")

        import ctypes
        from ctypes import wintypes

        class _DataBlob(ctypes.Structure):
            _fields_ = [
                ("cbData", wintypes.DWORD),
                ("pbData", ctypes.POINTER(ctypes.c_char)),
            ]

        self._ctypes = ctypes
        self._DataBlob = _DataBlob
        self._crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
        self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._crypt32.CryptProtectData.restype = wintypes.BOOL
        self._crypt32.CryptUnprotectData.restype = wintypes.BOOL
        self._kernel32.LocalFree.argtypes = [wintypes.HLOCAL]
        self._kernel32.LocalFree.restype = wintypes.HLOCAL

    def _in_blob(self, data: bytes):
        ctypes = self._ctypes
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        blob = self._DataBlob(
            len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char))
        )
        return blob, buf  # returned buffer must outlive the DPAPI call

    def _out_bytes(self, blob) -> bytes:
        try:
            return self._ctypes.string_at(blob.pbData, blob.cbData)
        finally:
            self._kernel32.LocalFree(blob.pbData)

    def _call(self, func, data: bytes, entropy: bytes, what: str) -> bytes:
        ctypes = self._ctypes
        din, _keep_d = self._in_blob(data)
        ein, _keep_e = self._in_blob(entropy)
        dout = self._DataBlob()
        ok = func(
            ctypes.byref(din),
            None,
            ctypes.byref(ein) if entropy else None,
            None,
            None,
            _CRYPTPROTECT_UI_FORBIDDEN,
            ctypes.byref(dout),
        )
        if not ok:
            raise KeystoreError(
                f"{what} failed (WinError {ctypes.get_last_error()})"
            )
        return self._out_bytes(dout)

    def protect(self, data: bytes, entropy: bytes = b"") -> bytes:
        return self._call(
            self._crypt32.CryptProtectData, data, entropy, "CryptProtectData"
        )

    def unprotect(self, blob: bytes, entropy: bytes = b"") -> bytes:
        return self._call(
            self._crypt32.CryptUnprotectData, blob, entropy, "CryptUnprotectData"
        )


def is_secure_store_available(platform: Optional[str] = None) -> bool:
    """Whether a supported at-rest secret store exists on this platform."""
    return (platform if platform is not None else sys.platform) == "win32"


def default_protector(platform: Optional[str] = None) -> SecretProtector:
    """Return the platform's default protector, or raise if none is supported."""
    if not is_secure_store_available(platform):
        raise KeystoreUnavailable(
            "The warp hot-wallet key needs Windows DPAPI for at-rest "
            "encryption; this platform has no supported secure store. Run the "
            "GUI on Windows to use the bridge."
        )
    return DpapiProtector()


# --------------------------------------------------------------------------- #
# Generic protect / unprotect (base64 envelope over the protector).
# --------------------------------------------------------------------------- #

def protect_secret(
    raw: bytes,
    *,
    extra_entropy: bytes = b"",
    protector: Optional[SecretProtector] = None,
) -> str:
    """Protect ``raw`` and return a base64 string safe to store in YAML/SQLite.

    ``extra_entropy`` is appended to the application entropy, letting a caller
    bind a blob to additional context (e.g. an ephemeral key to its job id).
    """
    p = protector or default_protector()
    blob = p.protect(raw, _APP_ENTROPY + extra_entropy)
    return base64.b64encode(blob).decode("ascii")


def unprotect_secret(
    blob_b64: str,
    *,
    extra_entropy: bytes = b"",
    protector: Optional[SecretProtector] = None,
) -> bytes:
    """Inverse of :func:`protect_secret`. Raises :class:`KeystoreError` on any
    corruption (bad base64, wrong account/entropy, tampered ciphertext)."""
    p = protector or default_protector()
    try:
        blob = base64.b64decode(blob_b64.encode("ascii"), validate=True)
    except (binascii.Error, ValueError) as exc:
        raise KeystoreError("stored secret is not valid base64") from exc
    return p.unprotect(blob, _APP_ENTROPY + extra_entropy)


# --------------------------------------------------------------------------- #
# EVM hot-wallet key (secp256k1).
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class EvmKey:
    """A Base/EVM private key and its EIP-55 checksummed address."""

    private_key: bytes  # 32 bytes
    address: str        # "0x" + 40 hex, checksummed

    def __post_init__(self) -> None:
        if len(self.private_key) != 32:
            raise KeystoreError(
                f"EVM private key must be 32 bytes, got {len(self.private_key)}"
            )

    def __repr__(self) -> str:
        # The generated dataclass repr renders private_key=b'\x..' in full.
        # WarpEngine._guarded pipes arbitrary exception text into both the log
        # and the GUI snapshot, so one traceback holding this object is all it
        # would take.
        return f"EvmKey(address={self.address})"

    __str__ = __repr__


def new_evm_key() -> EvmKey:
    """Generate a fresh random EVM hot-wallet key."""
    from eth_account import Account

    acct = Account.create()
    return EvmKey(bytes(acct.key), acct.address)


def evm_key_from_hex(hex_str: str) -> EvmKey:
    """Import an EVM key from a hex string (``0x`` prefix optional)."""
    from eth_account import Account

    s = hex_str.strip()
    if s[:2].lower() == "0x":
        s = s[2:]
    try:
        raw = bytes.fromhex(s)
    except ValueError as exc:
        raise KeystoreError("EVM private key is not valid hex") from exc
    if len(raw) != 32:
        raise KeystoreError(
            f"EVM private key must be 32 bytes, got {len(raw)}"
        )
    acct = Account.from_key(raw)
    return EvmKey(raw, acct.address)


def protect_evm_key(
    key: EvmKey, *, protector: Optional[SecretProtector] = None
) -> str:
    """Protect an EVM key for storage under ``warp.evm_private_key_dpapi``."""
    return protect_secret(key.private_key, protector=protector)


def load_evm_key(
    blob_b64: str, *, protector: Optional[SecretProtector] = None
) -> EvmKey:
    """Recover an EVM key from its stored blob, re-deriving the address."""
    from eth_account import Account

    raw = unprotect_secret(blob_b64, protector=protector)
    if len(raw) != 32:
        raise KeystoreError(f"EVM private key must be 32 bytes, got {len(raw)}")
    # Deliberately not via evm_key_from_hex: that round-trips the key through an
    # immutable str purely to parse it straight back, and Python cannot zero a
    # str -- so the secret would linger on the heap, and in any crash dump, for
    # the life of the process.
    return EvmKey(raw, Account.from_key(raw).address)


# --------------------------------------------------------------------------- #
# Ephemeral BLS key (per claim job).
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class BlsKey:
    """A BLS private key and its G1 public key (both raw bytes)."""

    private_key: bytes  # 32 bytes
    public_key: bytes   # 48 bytes (G1Element)

    def __post_init__(self) -> None:
        if len(self.private_key) != 32:
            raise KeystoreError(
                f"BLS private key must be 32 bytes, got {len(self.private_key)}"
            )

    def __repr__(self) -> str:
        # See EvmKey.__repr__. The public key alone is enough to identify which
        # ephemeral key this is.
        return f"BlsKey(public_key={self.public_key.hex()[:16]}...)"

    __str__ = __repr__


def new_bls_key() -> BlsKey:
    """Generate a fresh ephemeral BLS key via ``AugSchemeMPL.key_gen``."""
    import chia_rs

    sk = chia_rs.AugSchemeMPL.key_gen(os.urandom(32))
    return BlsKey(bytes(sk), bytes(sk.get_g1()))


def bls_key_from_bytes(raw: bytes) -> BlsKey:
    """Rebuild a BLS key (and its public key) from stored private bytes."""
    import chia_rs

    if len(raw) != 32:
        raise KeystoreError(f"BLS private key must be 32 bytes, got {len(raw)}")
    sk = chia_rs.PrivateKey.from_bytes(raw)
    return BlsKey(raw, bytes(sk.get_g1()))


def protect_bls_key(
    key: BlsKey,
    *,
    extra_entropy: bytes = b"",
    protector: Optional[SecretProtector] = None,
) -> str:
    """Protect an ephemeral BLS key (optionally bound to ``extra_entropy``)."""
    return protect_secret(
        key.private_key, extra_entropy=extra_entropy, protector=protector
    )


def load_bls_key(
    blob_b64: str,
    *,
    extra_entropy: bytes = b"",
    protector: Optional[SecretProtector] = None,
) -> BlsKey:
    """Recover an ephemeral BLS key from its stored blob."""
    raw = unprotect_secret(
        blob_b64, extra_entropy=extra_entropy, protector=protector
    )
    return bls_key_from_bytes(raw)
