"""[OPTIONS-FENCE] The options sub-account's coins, kept out of the app's own wallet spends.

With ``options.wallet.engine_fence`` the options address is a sub-account of
the trading wallet: the engine sends its coins as ``excluded_coin_ids`` on
every wallet call that selects coins (``cpp/include/xop/rpc/coin_fence.hpp``).
The app spends from the same wallet too -- bridge sends and claims, Dexie
takes, the options book's own top-ups -- through
:class:`gui.services.warp.wallet.WalletClient`. This module gives that client
the same list, so nothing the app sends can use a coin of the sub-account.

Which coins: every spendable coin of the wallet whose puzzle hash is the
options address's, or a CAT's wrapping of it (``chia_wallet_sdk``'s
``cat_puzzle_hash``, the one the book builds with). Listed from the wallet
itself, so a CAT wallet opened later is covered at the next refresh.

Fail-closed: a fence that is on but cannot list its coins raises
:class:`FenceUnavailableError`, and the client refuses the spend.
"""

from __future__ import annotations

import time
from collections.abc import Callable
from typing import Any

#: Wallet RPC endpoints that make the wallet select coins.
SPENDING = frozenset(
    {
        "send_transaction",
        "send_transaction_multi",
        "cat_spend",
        "take_offer",
        "create_offer_for_ids",
        "cancel_offer",
        "cancel_offers",
        "split_coins",
        "combine_coins",
        "select_coins",
        "create_signed_transaction",
    }
)

#: Wallet types whose coins can sit at the options address.
_STANDARD_WALLET = 0
_CAT_WALLET = 6

WalletRpc = Callable[[str, dict], dict]


class FenceUnavailableError(RuntimeError):
    """The fence is on and its coins are not known: nothing may be spent."""


def fenced_address(root: Any) -> tuple[str, str] | None:
    """``(address, prefix)`` when ``options.wallet.engine_fence`` is on, else None.

    Read straight from the config mapping, without the options parser: a
    bridge send must not fail on an options setting it has nothing to do
    with. On with no address is still on (an empty address, which the fence
    refuses to list).
    """
    if not isinstance(root, dict):
        return None
    options = root.get("options")
    if not isinstance(options, dict) or options.get("enabled") is not True:
        return None
    wallet = options.get("wallet")
    if not isinstance(wallet, dict):
        return None
    if wallet.get("engine_fence") is not True or wallet.get("share_trading_wallet") is not True:
        return None
    prefix = "txch" if str(options.get("network") or "mainnet").strip() == "testnet11" else "xch"
    return str(wallet.get("address") or "").strip(), prefix


def _sdk() -> Any:
    import chia_wallet_sdk

    return chia_wallet_sdk


def _unhex(value: Any) -> bytes:
    text = str(value or "")
    return bytes.fromhex(text[2:] if text[:2].lower() == "0x" else text)


class WalletFence:
    """The sub-account's coin ids, listed from the wallet and kept for ``max_age``."""

    def __init__(
        self,
        rpc: WalletRpc,
        address: str,
        prefix: str = "xch",
        *,
        max_age: float = 15.0,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._rpc = rpc
        self._address = address
        self._prefix = prefix
        self._max_age = max_age
        self._clock = clock
        self._ids: list[str] | None = None
        self._at = 0.0

    def puzzle_hash(self) -> bytes:
        try:
            decoded = _sdk().Address.decode(self._address)
        except Exception as exc:  # noqa: BLE001 - any decode failure fails closed
            raise FenceUnavailableError(
                f"options.wallet.address {self._address!r} is not valid"
            ) from exc
        if decoded.prefix != self._prefix:
            raise FenceUnavailableError(
                f"options.wallet.address is a {decoded.prefix} address; expected {self._prefix}"
            )
        return bytes(decoded.puzzle_hash)

    def coin_ids(self) -> list[str]:
        """0x-hex ids of every spendable coin at the sub-account. Raises FenceUnavailableError."""
        now = self._clock()
        if self._ids is not None and now - self._at <= self._max_age:
            return list(self._ids)
        try:
            ids = self._list()
        except FenceUnavailableError:
            self._ids = None
            raise
        except Exception as exc:  # noqa: BLE001 - fail closed on anything
            self._ids = None
            raise FenceUnavailableError(
                f"the options sub-account's coins could not be listed: {exc}"
            ) from exc
        self._ids, self._at = ids, now
        return list(ids)

    def _list(self) -> list[str]:
        sdk = _sdk()
        inner = self.puzzle_hash()
        answer = self._rpc("get_wallets", {})
        ids: list[str] = []
        for wallet in answer.get("wallets") or []:
            kind = int(wallet.get("type", -1))
            if kind not in (_STANDARD_WALLET, _CAT_WALLET):
                continue
            wallet_id = int(wallet["id"])
            if kind == _STANDARD_WALLET:
                wanted = inner
            else:
                asset = self._rpc("cat_get_asset_id", {"wallet_id": wallet_id}).get("asset_id")
                wanted = bytes(sdk.cat_puzzle_hash(_unhex(asset), inner))
            coins = self._rpc("get_spendable_coins", {"wallet_id": wallet_id})
            for record in coins.get("confirmed_records") or []:
                coin = record.get("coin", record)
                puzzle_hash = _unhex(coin.get("puzzle_hash"))
                if puzzle_hash != wanted:
                    continue
                made = sdk.Coin(
                    _unhex(coin.get("parent_coin_info")), puzzle_hash, int(coin["amount"])
                )
                ids.append("0x" + bytes(made.coin_id()).hex())
        return ids


def merge_exclusions(body: dict, ids: list[str]) -> dict:
    """``body`` with ``ids`` merged into its top-level ``excluded_coin_ids`` (a copy)."""
    if not ids:
        return body
    out = dict(body)
    seen: set[str] = set()
    merged: list[str] = []
    for item in list(out.get("excluded_coin_ids") or []) + list(ids):
        text = str(item).lower()
        text = text if text.startswith("0x") else "0x" + text
        if text not in seen:
            seen.add(text)
            merged.append(text)
    out["excluded_coin_ids"] = merged
    return out
