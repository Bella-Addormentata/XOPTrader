"""Which signatures a set of coin spends needs, and checking the ones we get.

Signing itself happens elsewhere (the reference wallet, see
:mod:`gui.services.options.reference_wallet`). This module is the part XOPTrader
does for itself, with no key and no network:

* :func:`required_signatures` runs each puzzle and lists every
  ``(public key, message)`` pair the chain will demand.
* :func:`verify` checks an aggregated signature against that list.

Together they bracket any signer: we know what we are asking it to sign before
we ask, and we know whether what came back is complete before we use it. A
signer that returned a signature for something else, or for only part of the
bundle, is caught here rather than by a rejected transaction.

:class:`LocalKeySigner` signs with keys held in memory. It exists for tests and
the simulator; production code has no secret key to give it.
"""

from __future__ import annotations

import dataclasses
import hashlib
from collections.abc import Iterable, Sequence
from typing import TYPE_CHECKING, Any, Protocol

if TYPE_CHECKING:
    from chia_wallet_sdk import CoinSpend, SecretKey

# AGG_SIG_ME appends the network's genesis challenge to every signed message, so
# a signature made for one network is useless on another.
MAINNET_AGG_SIG_ME_ADDITIONAL_DATA = bytes.fromhex(
    "ccd5bb71183532bff220ba46c268991a3ff07eb358e8255a65c30a2dce0e5fbb"
)
# The SDK simulator (and testnet11) use this one.
TESTNET11_AGG_SIG_ME_ADDITIONAL_DATA = bytes.fromhex(
    "37a90eb5185a9c4439a91ddc98bbadce7b4feba060d50116a067de66bf236615"
)

# Consensus MAX_BLOCK_COST_CLVM: the most a single puzzle may cost to run.
_MAX_COST = 11_000_000_000

_AGG_SIG_ME = 50
# AGG_SIG_PARENT .. AGG_SIG_UNSAFE. The standard puzzle, the CAT puzzle and the
# option puzzles only ever emit AGG_SIG_ME, so any of these in a bundle we built
# means the bundle is not what we think it is.
_OTHER_AGG_SIG = frozenset(range(43, 50))

# A BLS12-381 G2 point at infinity: the aggregate of no signatures.
_EMPTY_SIGNATURE = bytes([0xC0]) + bytes(95)


class SigningError(RuntimeError):
    """A bundle could not be signed, or a signature did not check out."""


def _sdk() -> Any:
    try:
        import chia_wallet_sdk
    except ImportError as exc:  # pragma: no cover - exercised only without the wheel
        raise SigningError(
            "chia-wallet-sdk is not installed; option contracts are unavailable"
        ) from exc
    return chia_wallet_sdk


@dataclasses.dataclass(frozen=True)
class RequiredSignature:
    """One ``AGG_SIG_ME`` the chain will check: ``message`` signed by ``public_key``.

    ``message`` is the full signed message: the puzzle's own message followed by
    the coin id and the network's additional data.
    """

    public_key: bytes
    message: bytes

    @property
    def hook(self) -> bytes:
        """The id the reference wallet's signer protocol gives this signature."""
        return hashlib.sha256(self.public_key + self.message).digest()


def required_signatures(
    coin_spends: Iterable[CoinSpend], agg_sig_me_additional_data: bytes
) -> list[RequiredSignature]:
    """List every signature ``coin_spends`` needs, in bundle order.

    Raises :class:`SigningError` if a puzzle fails to run or asks for a kind of
    signature other than ``AGG_SIG_ME``.
    """
    if len(agg_sig_me_additional_data) != 32:
        raise SigningError("agg_sig_me_additional_data must be 32 bytes")
    sdk = _sdk()
    clvm = sdk.Clvm()
    required: list[RequiredSignature] = []
    for coin_spend in coin_spends:
        coin_id = bytes(coin_spend.coin.coin_id())
        try:
            puzzle = clvm.deserialize(bytes(coin_spend.puzzle_reveal))
            solution = clvm.deserialize(bytes(coin_spend.solution))
            conditions = puzzle.run(solution, _MAX_COST, False).value.to_list()
        except Exception as exc:
            raise SigningError(f"coin {coin_id.hex()}: the puzzle did not run: {exc}") from exc
        if conditions is None:
            raise SigningError(f"coin {coin_id.hex()}: the puzzle did not return a condition list")
        for condition in conditions:
            opcode = _opcode(condition)
            if opcode in _OTHER_AGG_SIG:
                raise SigningError(
                    f"coin {coin_id.hex()}: unexpected signature condition {opcode}; "
                    "only AGG_SIG_ME is supported"
                )
            if opcode != _AGG_SIG_ME:
                continue
            parsed = condition.parse_agg_sig_me()
            if parsed is None:
                raise SigningError(f"coin {coin_id.hex()}: malformed AGG_SIG_ME condition")
            required.append(
                RequiredSignature(
                    public_key=bytes(parsed.public_key.to_bytes()),
                    message=bytes(parsed.message) + coin_id + bytes(agg_sig_me_additional_data),
                )
            )
    return required


def _opcode(condition: Any) -> Any:
    """A condition's opcode, or None if it is not a ``(opcode . args)`` pair."""
    try:
        return condition.first().to_int()
    except Exception:  # an atom, or a non-integer head: not a condition we sign for
        return None


def verify(required: Sequence[RequiredSignature], aggregated_signature: bytes) -> bool:
    """True if ``aggregated_signature`` is exactly the aggregate ``required`` needs."""
    sdk = _sdk()
    try:
        signature = sdk.Signature.from_bytes(bytes(aggregated_signature))
    except Exception:
        return False
    if not required:
        return bool(signature.is_infinity())
    try:
        keys = [sdk.PublicKey.from_bytes(item.public_key) for item in required]
        return bool(
            sdk.PublicKey.aggregate_verify(keys, [item.message for item in required], signature)
        )
    except Exception:
        return False


def empty_signature() -> bytes:
    """The aggregate of no signatures, for a bundle that needs none."""
    return _EMPTY_SIGNATURE


class Signer(Protocol):
    """Anything that can sign coin spends we built."""

    def sign(self, coin_spends: Sequence[CoinSpend]) -> bytes:
        """Return the aggregated signature (96 bytes) for ``coin_spends``."""
        ...


class LocalKeySigner:
    """Signs with synthetic secret keys held in memory. Tests and simulator only."""

    def __init__(self, secret_keys: Iterable[SecretKey], agg_sig_me_additional_data: bytes) -> None:
        self._additional_data = bytes(agg_sig_me_additional_data)
        self._keys = {bytes(key.public_key().to_bytes()): key for key in secret_keys}

    def sign(self, coin_spends: Sequence[CoinSpend]) -> bytes:
        sdk = _sdk()
        signatures = []
        for item in required_signatures(coin_spends, self._additional_data):
            key = self._keys.get(item.public_key)
            if key is None:
                raise SigningError(f"no secret key for public key {item.public_key.hex()}")
            signatures.append(key.sign(item.message))
        if not signatures:
            return empty_signature()
        return bytes(sdk.Signature.aggregate(signatures).to_bytes())
