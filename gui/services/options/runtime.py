"""Wiring: turn config.yaml into a running options book.

Everything else in this package takes its clients as arguments. This module is
the one place that builds the real ones: the dedicated wallet's RPC, a chain
reader over the operator's own full node or, failing that, coinset.org, the
desk client and the market source.

``python -m gui.services.options`` (see :mod:`__main__`) drives it from a
terminal: ``status``, ``step``, ``run`` and ``withdraw-all``.
"""

from __future__ import annotations

import logging
import time
from collections.abc import Callable
from typing import Any

from gui.services.options import (
    chain,
    config,
    desk,
    engine_guard,
    market,
    signing,
    splash,
    store,
)
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
_WALLET_LIST_SECONDS = 300
_ADDRESS_KEY = "options_address"


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
        clock: Callable[[], float] = time.time,
        memory: Any = None,
        fingerprint: int | None = None,
    ) -> None:
        self._rpc = rpc
        self._chain = chain_reader
        self._prefix = prefix
        self._configured = address
        self._clock = clock
        self._memory = memory  # the book's store: recall(key) / remember(key, value)
        # An address belongs to a key, so it is remembered under the key's name.
        self._address_key = _ADDRESS_KEY if fingerprint is None else f"{_ADDRESS_KEY}:{fingerprint}"
        self._puzzle_hash: bytes | None = None
        self._cat_wallets: dict[bytes, int] = {}
        self._cat_wallets_at: float | None = None

    def known_address(self) -> str:
        """The address set in the config or used before, or "" if neither. Asks nobody."""
        if self._configured:
            return self._configured
        if self._memory is None:
            return ""
        return str(self._memory.recall(self._address_key) or "")

    def puzzle_hash(self) -> bytes:
        if self._puzzle_hash is None:
            # The address this book has used before, if any. The wallet's
            # current address moves on whenever anything asks it for a new one,
            # and a book that followed it would leave its funds, and the keys
            # to its own options, behind.
            address = self.known_address()
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
            if self._memory is not None and self._memory.recall(self._address_key) != address:
                self._memory.remember(self._address_key, address)
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
        now = self._clock()
        listed = self._cat_wallets_at
        # A wallet for the asset can appear later (the first premium creates
        # one), so a miss is looked up again, but not on every step.
        stale = listed is None or now - listed >= _WALLET_LIST_SECONDS
        if asset_id not in self._cat_wallets and stale:
            self._cat_wallets_at = now
            answer = self._rpc("get_wallets", {"type": _CAT_WALLET_TYPE})
            for wallet in answer.get("wallets") or []:
                try:
                    wallet_id = int(wallet["id"])
                    found = self._rpc("cat_get_asset_id", {"wallet_id": wallet_id})
                    self._cat_wallets[bytes.fromhex(str(found["asset_id"]))] = wallet_id
                except (KeyError, TypeError, ValueError):
                    continue
        return self._cat_wallets.get(asset_id)

    def _cat_coins(self, asset_id: bytes) -> list[Any]:
        wallet_id = self._cat_wallet(asset_id)
        if wallet_id is None:
            return []
        wrapped = bytes(_sdk().cat_puzzle_hash(bytes(asset_id), self.puzzle_hash()))
        return [coin for coin in self._spendable(wallet_id) if bytes(coin.puzzle_hash) == wrapped]

    def cats(self, asset_id: bytes) -> list[Any]:
        # The wallet already knows the coin; only its parent's spend, which is
        # settled and so read once, is needed from the chain.
        return [chain.load_cat_coin(self._chain, coin) for coin in self._cat_coins(asset_id)]

    def held(self, asset_ids: Any) -> frozenset[bytes]:
        """Ids of the spendable coins at the address. The chain is not asked."""
        ids = {bytes(coin.coin_id()) for coin in self.xch_coins()}
        for asset_id in asset_ids:
            ids.update(bytes(coin.coin_id()) for coin in self._cat_coins(bytes(asset_id)))
        return frozenset(ids)


def _url_host(host: str) -> str:
    """``host`` as it must appear in a URL: an IPv6 address goes in brackets."""
    host = host.strip()
    return f"[{host}]" if ":" in host and not host.startswith("[") else host


def wallet_rpc(cfg: config.OptionsConfig) -> WalletRpc:
    """The dedicated wallet's RPC as an ``(endpoint, body)`` callable."""
    from gui.services.warp.wallet import WalletClient

    client = WalletClient(
        {
            "host": _url_host(cfg.wallet.host),
            "port": cfg.wallet.port,
            "fingerprint": cfg.wallet.fingerprint,
            "cert_path": cfg.wallet.cert_path,
            "key_path": cfg.wallet.key_path,
        },
        timeout=30.0,
    )
    return client._post  # noqa: SLF001 - WalletClient's one generic call


def _node_poster(cert: str, key: str, timeout: float, ca: str = "") -> Callable[[str, dict], dict]:
    """HTTPS to a Chia full node's RPC, with our client certificate.

    The client certificate proves who we are to the node. Whether the node is
    who it should be is a separate question, answered by ``ca``: the node's own
    certificate authority (``config/ssl/ca/private_ca.crt``). With it, the
    node's certificate must be signed by that authority and by nothing else.
    The host name is not compared, because Chia issues every certificate to
    "Chia" and never to a host; this is how Chia's own clients check each other.

    Without ``ca`` the node's certificate is not checked at all, which
    ``config`` allows only for a node on this machine, where nothing stands
    between the two.
    """
    import requests  # lazy: only when a real network call is made

    session = requests.Session()
    if not ca:
        import urllib3

        urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

        def post(url: str, body: dict) -> dict:
            response = session.post(
                url,
                json=body,
                cert=(cert, key),
                verify=False,  # noqa: S501 - this machine only; see the docstring
                timeout=timeout,
            )
            response.raise_for_status()
            return response.json()

        return post

    import ssl

    from requests.adapters import HTTPAdapter

    # Only this authority: a context made with a cafile loads no system roots.
    context = ssl.create_default_context(cafile=ca)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_REQUIRED
    # Chia's authority certificate carries no key-usage extension, which the
    # strict checking Python 3.13 turns on by default refuses outright. Chia's
    # own clients do not check strictly either; the signature still has to be
    # that authority's.
    context.verify_flags &= ~ssl.VERIFY_X509_STRICT
    context.load_cert_chain(cert, key)

    class _PrivateCa(HTTPAdapter):
        def init_poolmanager(self, *args: Any, **kwargs: Any) -> Any:
            kwargs["ssl_context"] = context
            kwargs["assert_hostname"] = False
            return super().init_poolmanager(*args, **kwargs)

    session.mount("https://", _PrivateCa())

    def checked(url: str, body: dict) -> dict:
        # verify names the same file, so that nothing else is added to what
        # the context trusts.
        response = session.post(url, json=body, verify=ca, timeout=timeout)
        response.raise_for_status()
        return response.json()

    return checked


def chain_source(cfg: config.OptionsConfig) -> str:
    """Where chain reads go, for the operator to see."""
    if cfg.node.enabled:
        checked = "certificate checked" if cfg.node.ca_cert_path else "on this machine"
        where = f"{_url_host(cfg.node.host)}:{cfg.node.port}"
        return f"your full node at {where} ({checked}; coinset is not used)"
    return f"coinset ({cfg.coinset_url or _COINSET[cfg.network]})"


def chain_reader(cfg: config.OptionsConfig) -> chain.NodeChainReader:
    """The operator's own full node if ``options.node`` names one, else coinset.

    The two speak the same RPC, so one client serves both; only the address
    and the client certificate differ. With a node configured, coinset is never
    contacted.
    """
    from gui.services.warp.coinset import CoinsetClient

    if cfg.node.enabled:
        client = CoinsetClient(
            f"https://{_url_host(cfg.node.host)}:{cfg.node.port}",
            poster=_node_poster(cfg.node.cert_path, cfg.node.key_path, 30.0, cfg.node.ca_cert_path),
        )
    else:
        client = CoinsetClient(cfg.coinset_url or _COINSET[cfg.network])
    return chain.NodeChainReader(client)


def shared_wallet_guard(cfg: config.OptionsConfig, wallet: Any) -> Any:
    """``wallet``, checked for a running engine at every sign and push when shared."""
    if not cfg.wallet.share_trading_wallet:
        return wallet
    return engine_guard.EngineCheckedWallet(wallet, cfg.wallet.engine_metrics_url)


def build(
    cfg: config.OptionsConfig,
    *,
    rpc: WalletRpc | None = None,
    reader: chain.ChainReader | None = None,
    market_reader: Callable[[], market.Market] | None = None,
    splash_client: Any = None,
    clock: Callable[[], float] = time.time,
) -> tuple[OptionsBook, store.OptionStore, WalletCoinSource]:
    """Assemble the book from config. The network-facing parts can be injected."""
    rpc = rpc or wallet_rpc(cfg)
    book_store = store.OptionStore(cfg.database_path)
    # One cache for the book and the coin source, kept in the book's database.
    reader = chain.CachedChain.over(reader or chain_reader(cfg), book_store, clock=clock)
    wallet = shared_wallet_guard(
        cfg,
        ReferenceWallet(
            rpc,
            agg_sig_me_additional_data=_ADDITIONAL_DATA[cfg.network],
            address_prefix=cfg.address_prefix,
            fingerprint=cfg.wallet.fingerprint,
        ),
    )
    coins = WalletCoinSource(
        rpc,
        reader,
        address=cfg.wallet.address,
        prefix=cfg.address_prefix,
        clock=clock,
        memory=book_store,
        fingerprint=cfg.wallet.fingerprint,
    )
    if market_reader is None:
        market_reader = market.MarketSource(
            cfg.quote_asset_id,
            hedge_size_xch=cfg.market.hedge_size_xch,
            coingecko_api_key=cfg.coingecko_api_key,
        ).read
    # The site lists mainnet options only.
    listing = desk.DeskClient() if cfg.network == config.MAINNET else None
    # Splash has a network of its own for each chain; which one the daemon is
    # on is the daemon's --testnet flag, not something it can be asked.
    announcer = None
    if cfg.splash.enabled:
        announcer = splash_client or splash.SplashClient(cfg.splash.submit_url)
    book = OptionsBook(
        cfg,
        book_store=book_store,
        wallet=wallet,
        coins=coins,
        chain_reader=reader,
        market=market_reader,
        desk=listing,
        splash=announcer,
        clock=clock,
    )
    return book, book_store, coins
