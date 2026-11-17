"""The Chia reference wallet as the signer for spends XOPTrader builds itself.

The reference wallet cannot build an option spend, but it can sign one. Its
signer RPCs (the CHIP-0028/0029 signer protocol) take arbitrary coin spends:

``gather_signing_info``
    Runs every puzzle, collects the ``AGG_SIG`` conditions, and returns one
    signing *target* per signature plus *hints* that say how to derive each key
    it recognises. It recognises a key when that key's standard puzzle hash is
    one of the wallet's own addresses -- whatever puzzle asked for it.

``execute_signing_instructions``
    Signs the targets with the wallet's keys.

Every signature in an option's life is by the standard key of an ordinary
address (see :mod:`gui.services.options.builder`), so if those addresses are the
wallet's, the wallet can sign all of it and XOPTrader never holds a key.

WHAT WE DO NOT TRUST. ``execute_signing_instructions`` signs whatever targets it
is handed, so this module never forwards a target it did not derive itself:
the wallet's targets must match :func:`signing.required_signatures` exactly, and
the signature that comes back must verify against the same list before it is
returned. That guards against a confused or substituted RPC endpoint. It does
not review the spends -- deciding that a bundle does what was intended is the
caller's job, before :meth:`ReferenceWallet.sign` is called.

Checked against chia-blockchain 2.7.5 (the JSON below is that release's). Its
changelog lists "Add support for external signers" and "Conform to CHIP-0029"
under 2.4.3 (2024-08-21), so that is the oldest wallet expected to have the
signer RPCs; no older release was tried. A wallet without them answers with an
unknown-endpoint error, which surfaces as :class:`SigningError`.
"""

from __future__ import annotations

import collections
from collections.abc import Callable, Sequence
from typing import TYPE_CHECKING, Any

from gui.services.options.signing import (
    MAINNET_AGG_SIG_ME_ADDITIONAL_DATA,
    RequiredSignature,
    SigningError,
    empty_signature,
    required_signatures,
    verify,
)

if TYPE_CHECKING:
    from chia_wallet_sdk import Coin, CoinSpend, PublicKey

# ``(endpoint, json_body) -> response dict``; raises when the wallet reports
# failure. ``gui.services.warp.wallet.WalletClient._post`` has this shape.
WalletRpc = Callable[[str, dict], dict]

# Signed (under CHIP-0002's own domain separation, so it can never double as a
# transaction signature) only to make the wallet reveal an address's key.
_KEY_LOOKUP_MESSAGE = "XOPTrader option key lookup"

# Open offers asked of the wallet per page, and the most pages read: far more
# offers than one wallet keeps open.
_OFFER_PAGE = 1000
_MAX_OFFER_PAGES = 20


def _sdk() -> Any:
    try:
        import chia_wallet_sdk
    except ImportError as exc:  # pragma: no cover - exercised only without the wheel
        raise SigningError(
            "chia-wallet-sdk is not installed; option contracts are unavailable"
        ) from exc
    return chia_wallet_sdk


def _unhex(value: Any, what: str) -> bytes:
    if not isinstance(value, str):
        raise SigningError(f"wallet returned a non-string {what}")
    text = value[2:] if value[:2].lower() == "0x" else value
    try:
        return bytes.fromhex(text)
    except ValueError as exc:
        raise SigningError(f"wallet returned a malformed {what}") from exc


def _0x(value: bytes) -> str:
    return "0x" + bytes(value).hex()


def spend_to_json(coin_spend: CoinSpend) -> dict:
    """A coin spend in the signer protocol's ``Spend`` JSON shape."""
    coin = coin_spend.coin
    return {
        "coin": {
            "parent_coin_id": _0x(coin.parent_coin_info),
            "puzzle_hash": _0x(coin.puzzle_hash),
            "amount": int(coin.amount),
        },
        "puzzle": _0x(coin_spend.puzzle_reveal),
        "solution": _0x(coin_spend.solution),
    }


def coin_from_json(record: dict) -> Coin:
    """A wallet RPC coin (``parent_coin_info`` / ``puzzle_hash`` / ``amount``) as an SDK coin.

    Accepts either a bare coin or a coin record that wraps one under ``coin``.
    """
    coin = record.get("coin", record) if isinstance(record, dict) else None
    if not isinstance(coin, dict):
        raise SigningError("wallet returned a malformed coin")
    try:
        amount = int(coin["amount"])
    except (KeyError, TypeError, ValueError) as exc:
        raise SigningError("wallet returned a coin without an amount") from exc
    return _sdk().Coin(
        _unhex(coin.get("parent_coin_info"), "parent_coin_info"),
        _unhex(coin.get("puzzle_hash"), "puzzle_hash"),
        amount,
    )


class ReferenceWallet:
    """Key lookup, signing and broadcast through the reference wallet's RPC."""

    def __init__(
        self,
        rpc: WalletRpc,
        *,
        agg_sig_me_additional_data: bytes = MAINNET_AGG_SIG_ME_ADDITIONAL_DATA,
        address_prefix: str = "xch",
        fingerprint: int | None = None,
    ) -> None:
        self._rpc = rpc
        self._additional_data = bytes(agg_sig_me_additional_data)
        self._prefix = address_prefix
        self._fingerprint = fingerprint

    def _call(self, endpoint: str, body: dict) -> dict:
        try:
            response = self._rpc(endpoint, body)
        except SigningError:
            raise
        except Exception as exc:  # transport or success=false, per the WalletRpc contract
            raise SigningError(f"wallet {endpoint} failed: {exc}") from exc
        if not isinstance(response, dict):
            raise SigningError(f"wallet {endpoint} returned {type(response).__name__}")
        if response.get("success") is False:
            raise SigningError(f"wallet {endpoint} failed: {response.get('error') or 'unknown'}")
        return response

    # -- readiness ----------------------------------------------------------

    def not_ready(self, *, select_key: bool = True) -> str | None:
        """Why the wallet should not be acted on right now, or None if it can be.

        The daemon answers for whichever key is logged in, and another client
        may have changed that, so the configured fingerprint is selected first
        (the daemon does nothing if it is already the one). Then the wallet
        must call itself synced: one that is behind lists coins already spent
        and misses ones that have arrived. With no fingerprint configured the
        key in use is whichever is logged in.

        ``select_key=False`` is for looking without touching: the key is not
        changed, and a wallet open on another one is reported instead.
        """
        try:
            if self._fingerprint is not None and select_key:
                self._call("log_in", {"fingerprint": int(self._fingerprint)})
            elif self._fingerprint is not None:
                answer = self._call("get_logged_in_fingerprint", {})
                if answer.get("fingerprint") != int(self._fingerprint):
                    return (
                        f"the options wallet has another key open "
                        f"({answer.get('fingerprint')}); the next step will select "
                        f"{self._fingerprint}"
                    )
            status = self._call("get_sync_status", {})
        except SigningError as exc:
            return f"the options wallet is not answering ({exc})"
        if status.get("synced") is not True or status.get("syncing") is True:
            return "the options wallet is still syncing"
        return None

    # -- keys ---------------------------------------------------------------

    def synthetic_key(self, puzzle_hash: bytes) -> PublicKey:
        """The synthetic public key of one of the wallet's own addresses.

        The answer is self-certifying: the key's standard puzzle hash must be
        ``puzzle_hash``, so a wrong or substituted key is refused here.
        """
        sdk = _sdk()
        address = sdk.Address(bytes(puzzle_hash), self._prefix).encode()
        response = self._call(
            "sign_message_by_address", {"address": address, "message": _KEY_LOOKUP_MESSAGE}
        )
        try:
            key = sdk.PublicKey.from_bytes(_unhex(response.get("pubkey"), "pubkey"))
        except SigningError:
            raise
        except Exception as exc:
            raise SigningError("wallet returned an invalid public key") from exc
        if bytes(sdk.standard_puzzle_hash(key)) != bytes(puzzle_hash):
            raise SigningError("wallet returned a key that does not belong to the address")
        return key

    # -- signing ------------------------------------------------------------

    def sign(self, coin_spends: Sequence[CoinSpend]) -> bytes:
        """Have the wallet sign ``coin_spends``; returns the aggregated signature.

        Raises :class:`SigningError` if the wallet cannot sign every signature
        the bundle needs, proposes to sign anything else, or returns a signature
        that does not verify. Nothing is broadcast.
        """
        required = required_signatures(coin_spends, self._additional_data)
        if not required:
            return empty_signature()

        info = self._call(
            "gather_signing_info", {"spends": [spend_to_json(spend) for spend in coin_spends]}
        )
        instructions = info.get("signing_instructions")
        if not isinstance(instructions, dict):
            raise SigningError("wallet returned no signing instructions")
        self._check_targets(required, instructions.get("targets"))

        signed = self._call(
            "execute_signing_instructions",
            {"signing_instructions": instructions, "partial_allowed": False},
        )
        responses = signed.get("signing_responses")
        if not isinstance(responses, list) or not responses:
            raise SigningError("wallet returned no signatures")
        sdk = _sdk()
        expected_hooks = {item.hook for item in required}
        signatures = []
        for response in responses:
            if not isinstance(response, dict):
                raise SigningError("wallet returned a malformed signing response")
            if _unhex(response.get("hook"), "hook") not in expected_hooks:
                raise SigningError("wallet signed something that was not requested")
            try:
                signatures.append(
                    sdk.Signature.from_bytes(_unhex(response.get("signature"), "signature"))
                )
            except SigningError:
                raise
            except Exception as exc:
                raise SigningError("wallet returned an invalid signature") from exc
        aggregate = bytes(sdk.Signature.aggregate(signatures).to_bytes())
        if not verify(required, aggregate):
            raise SigningError("the wallet's signature does not cover the bundle")
        return aggregate

    @staticmethod
    def _check_targets(required: Sequence[RequiredSignature], targets: Any) -> None:
        """The wallet's signing targets must be exactly the signatures we derived."""
        if not isinstance(targets, list):
            raise SigningError("wallet returned no signing targets")
        ours = collections.Counter((item.hook, item.message) for item in required)
        theirs: collections.Counter = collections.Counter()
        for target in targets:
            if not isinstance(target, dict):
                raise SigningError("wallet returned a malformed signing target")
            theirs[
                (_unhex(target.get("hook"), "hook"), _unhex(target.get("message"), "message"))
            ] += 1
        if ours != theirs:
            raise SigningError(
                "the wallet's signing targets differ from the signatures the bundle needs"
            )

    # -- broadcast ----------------------------------------------------------

    def fee_rate(self) -> int | None:
        """What inclusion in the next block costs, in mojos per unit of cost.

        The wallet's ``get_fee_estimate`` passes on its full-node peer's
        estimate, so no other node has to be asked. It is a whole number: 0
        while the mempool has room, 1 or more once it is competing for space.
        None when the wallet cannot say (no peer, or a wallet older than the
        RPC), which a caller should treat as "do not guess".
        """
        try:
            answer = self._call("get_fee_estimate", {})
            rate = int(answer["fee_per_cost"])
        except (SigningError, KeyError, TypeError, ValueError):
            return None
        return rate if rate >= 0 else None

    def open_offer_ids(self) -> list[bytes]:
        """Trade ids of the offers this wallet made that are still open.

        With ``options.wallet.share_trading_wallet`` these are the engine's
        resting offers. The market view leaves them out of the Dexie range:
        they are not liquidity the book could ever trade against. Taken,
        cancelled and completed offers are not asked for. The wallet answers a
        page at a time; pages are read until one comes back short.
        """
        found: dict[bytes, None] = {}  # in the wallet's order, each once
        start = 0
        for _ in range(_MAX_OFFER_PAGES):
            answer = self._call(
                "get_all_offers",
                {
                    "start": start,
                    "end": start + _OFFER_PAGE,
                    "exclude_my_offers": False,
                    "exclude_taken_offers": True,
                    "include_completed": False,
                },
            )
            records = answer.get("trade_records") or []
            if not isinstance(records, list):
                raise SigningError("wallet get_all_offers returned no list of offers")
            before = len(found)
            for record in records:
                if isinstance(record, dict) and record.get("is_my_offer", True) is not False:
                    found.setdefault(_unhex(record.get("trade_id"), "trade_id"), None)
            # A page that adds nothing means a wallet that does not page: stop.
            if len(records) < _OFFER_PAGE or len(found) == before:
                break
            start += _OFFER_PAGE
        return list(found)

    def push(self, coin_spends: Sequence[CoinSpend], aggregated_signature: bytes) -> bytes:
        """Hand a signed bundle to the wallet's full-node peer.

        Refuses a bundle whose signature does not verify, so a signing mistake
        costs nothing. Returns the spend bundle's name (its mempool id).

        NO VERDICT COMES BACK. The wallet's ``push_tx`` forwards the bundle to
        its peer and returns; if the node rejects it (a coin already spent, a
        timelock not yet reached) this call still succeeds. Read the outcome
        with :func:`gui.services.options.chain.bundle_status`, or push through
        a full node's own ``push_tx``, which does answer.
        """
        required = required_signatures(coin_spends, self._additional_data)
        if not verify(required, aggregated_signature):
            raise SigningError("refusing to broadcast: the signature does not cover the bundle")
        sdk = _sdk()
        bundle = sdk.SpendBundle(
            list(coin_spends), sdk.Signature.from_bytes(bytes(aggregated_signature))
        )
        self._call("push_tx", {"spend_bundle": _0x(bundle.to_bytes())})
        return bytes(bundle.hash())
