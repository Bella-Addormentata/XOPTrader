"""Wiring: turn config.yaml into a running options book.

Everything else in this package takes its clients as arguments. This module is
the one place that builds the real ones: the dedicated wallet's RPC, a chain
reader over coinset, the desk client and the market source.

``python -m gui.services.options`` (see :mod:`__main__`) drives it from a
terminal: ``status``, ``step``, ``run`` and ``withdraw-all``.
"""

from __future__ import annotations

import logging
import time
from collections.abc import Callable
from typing import Any

from gui.services.options import chain, config, desk, market, signing, store
from gui.services.options.reference_wallet import ReferenceWallet, WalletRpc, coin_from_json
from gui.services.options.service import OptionsBook, OptionsError

log = logging.getLogger(__name__)

_COINSET = {
    config.MAINNET: "https://api.coinset.org",
    config.TESTNET11: "https://testnet11.api.coinset.org",
}
_ADDITIONAL_DATA = {
    config.MAINNET: signing.MAINNET_AGG_SIG_ME_ADDITIONAL_DATA,
    config.TESTNET11: signing.TESTNET11_AGG_SIG_ME_ADDITIONAL_DATA,
}
_XCH_WALLET_ID = 1
_CAT_WALLET_TYPE = 6


def _sdk() -> Any:
    try:
        import chia_wallet_sdk
    except ImportError as exc:  # pragma: no cover - exercised only without the wheel
        raise OptionsError(
            "chia-wallet-sdk is not installed; option contracts are unavailable"
        ) from exc
    return chia_wallet_sdk


class WalletCoinSource:
    """The options address and its spendable coins, from the wallet's RPC.

    Only coins at the one options address are used. XCH or CATs the wallet holds
    at its other addresses are left alone: send funds to the options address.
    """

    def __init__(
        self,
        rpc: WalletRpc,
        chain_reader: chain.ChainReader,
        *,
        address: str = "",
        prefix: str = "xch",
    ) -> None:
        self._rpc = rpc
        self._chain = chain_reader
        self._prefix = prefix
        self._configured = address
        self._puzzle_hash: bytes | None = None
        self._cat_wallets: dict[bytes, int] = {}

    def puzzle_hash(self) -> bytes:
        if self._puzzle_hash is None:
            address = self._configured
            if not address:
                answer = self._rpc(
                    "get_next_address", {"wallet_id": _XCH_WALLET_ID, "new_address": False}
                )
                address = str(answer.get("address") or "")
            try:
                decoded = _sdk().Address.decode(address)
            except Exception as exc:
                raise OptionsError(f"the options address {address!r} is not valid") from exc
            if decoded.prefix != self._prefix:
                raise OptionsError(
                    f"the options address is a {decoded.prefix} address; this network uses "
                    f"{self._prefix}"
                )
            self._puzzle_hash = bytes(decoded.puzzle_hash)
        return self._puzzle_hash

    def address(self) -> str:
        return str(_sdk().Address(self.puzzle_hash(), self._prefix).encode())

    def _spendable(self, wallet_id: int) -> list[Any]:
        answer = self._rpc("get_spendable_coins", {"wallet_id": int(wallet_id)})
        records = answer.get("confirmed_records") or []
        return [coin_from_json(record) for record in records]

    def xch_coins(self) -> list[Any]:
        mine = self.puzzle_hash()
        return [coin for coin in self._spendable(_XCH_WALLET_ID) if bytes(coin.puzzle_hash) == mine]

    def _cat_wallet(self, asset_id: bytes) -> int | None:
        asset_id = bytes(asset_id)
        if asset_id not in self._cat_wallets:
            answer = self._rpc("get_wallets", {"type": _CAT_WALLET_TYPE})
            for wallet in answer.get("wallets") or []:
                try:
                    wallet_id = int(wallet["id"])
                    found = self._rpc("cat_get_asset_id", {"wallet_id": wallet_id})
                    self._cat_wallets[bytes.fromhex(str(found["asset_id"]))] = wallet_id
                except (KeyError, TypeError, ValueError):
                    continue
        return self._cat_wallets.get(asset_id)

    def cats(self, asset_id: bytes) -> list[Any]:
        wallet_id = self._cat_wallet(asset_id)
        if wallet_id is None:
            return []
        wrapped = bytes(_sdk().cat_puzzle_hash(bytes(asset_id), self.puzzle_hash()))
        return [
            chain.load_cat(self._chain, coin.coin_id())
            for coin in self._spendable(wallet_id)
            if bytes(coin.puzzle_hash) == wrapped
        ]


def wallet_rpc(cfg: config.OptionsConfig) -> WalletRpc:
    """The dedicated wallet's RPC as an ``(endpoint, body)`` callable."""
    from gui.services.warp.wallet import WalletClient

    client = WalletClient(
        {
            "host": cfg.wallet.host,
            "port": cfg.wallet.port,
            "fingerprint": cfg.wallet.fingerprint,
            "cert_path": cfg.wallet.cert_path,
            "key_path": cfg.wallet.key_path,
        },
        timeout=30.0,
    )
    return client._post  # noqa: SLF001 - WalletClient's one generic call


def chain_reader(cfg: config.OptionsConfig) -> chain.NodeChainReader:
    from gui.services.warp.coinset import CoinsetClient

    return chain.NodeChainReader(CoinsetClient(cfg.coinset_url or _COINSET[cfg.network]))


def build(
    cfg: config.OptionsConfig,
    *,
    rpc: WalletRpc | None = None,
    reader: chain.ChainReader | None = None,
    market_reader: Callable[[], market.Market] | None = None,
    clock: Callable[[], float] = time.time,
) -> tuple[OptionsBook, store.OptionStore, WalletCoinSource]:
    """Assemble the book from config. The network-facing parts can be injected."""
    rpc = rpc or wallet_rpc(cfg)
    reader = reader or chain_reader(cfg)
    wallet = ReferenceWallet(
        rpc,
        agg_sig_me_additional_data=_ADDITIONAL_DATA[cfg.network],
        address_prefix=cfg.address_prefix,
    )
    coins = WalletCoinSource(rpc, reader, address=cfg.wallet.address, prefix=cfg.address_prefix)
    if market_reader is None:
        market_reader = market.MarketSource(
            cfg.quote_asset_id, hedge_size_xch=cfg.market.hedge_size_xch
        ).read
    # The site lists mainnet options only.
    listing = desk.DeskClient() if cfg.network == config.MAINNET else None
    book_store = store.OptionStore(cfg.database_path)
    book = OptionsBook(
        cfg,
        book_store=book_store,
        wallet=wallet,
        coins=coins,
        chain_reader=reader,
        market=market_reader,
        desk=listing,
        clock=clock,
    )
    return book, book_store, coins
