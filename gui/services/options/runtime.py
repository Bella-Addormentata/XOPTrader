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
import logging.handlers
import time
from collections.abc import Callable
from pathlib import Path
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

#: The book's own log file: rotated as the GUI's gui.log is, the time on every line.
LOG_BYTES = 2 * 1024 * 1024
LOG_BACKUPS = 3
LOG_FORMAT = "%(asctime)s %(levelname)-7s %(name)s: %(message)s"
LOG_DATE_FORMAT = "%Y-%m-%d %H:%M:%S%z"


def log_path(cfg: config.OptionsConfig) -> Path | None:
    """The book's log file: beside its database, named after it; None for an in-memory one."""
    if cfg.database_path == ":memory:":
        return None
    database = Path(cfg.database_path)
    if database.suffix == ".log":  # the log must not be the database itself
        return database.with_name(f"{database.name}.log")
    return database.with_suffix(".log")


def log_handler(path: Path) -> logging.Handler:
    """A handler writing info and up to ``path``, rotated. Opens the file on its first line."""
    path.parent.mkdir(parents=True, exist_ok=True)
    handler = logging.handlers.RotatingFileHandler(
        path, maxBytes=LOG_BYTES, backupCount=LOG_BACKUPS, encoding="utf-8", delay=True
    )
    handler.setLevel(logging.INFO)
    handler.setFormatter(logging.Formatter(LOG_FORMAT, LOG_DATE_FORMAT))
    return handler


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

    def wallet_xch_coins(self) -> list[Any]:
        """Every spendable XCH coin of the wallet, at any address (``fund_from_wallet``)."""
        return self._spendable(_XCH_WALLET_ID)

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
                    # chia-blockchain 2.7.4 answers "0x"-prefixed, which
                    # bytes.fromhex refuses: the asset's wallet was never found.
                    asset_hex = str(found["asset_id"]).strip().lower().removeprefix("0x")
                    self._cat_wallets[bytes.fromhex(asset_hex)] = wallet_id
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

    def wallet_cat_coins(self, asset_id: bytes) -> list[Any]:
        """Every spendable coin of one CAT in the wallet, any address, no lineage yet."""
        wallet_id = self._cat_wallet(asset_id)
        return [] if wallet_id is None else self._spendable(wallet_id)

    def cat_of(self, coin: Any) -> Any:
        """One coin from :meth:`wallet_cat_coins`, with its lineage (one settled read)."""
        return chain.load_cat_coin(self._chain, coin)

    # -- [OPTIONS-FENCE] top-ups ---------------------------------------------------

    def send_to_address(self, *, asset_id: bytes | None, amount: int, fee: int) -> str:
        """An ordinary wallet send of ``amount`` to the options address; its transaction id.

        The wallet picks the coins, so it skips those held for offers and
        those its own pending sends spend; with ``engine_fence`` the client's
        fence also keeps it off the options address's own coins.
        """
        address = self.address()
        if asset_id is None:
            answer = self._rpc(
                "send_transaction",
                {
                    "wallet_id": _XCH_WALLET_ID,
                    "amount": int(amount),
                    "address": address,
                    "fee": int(fee),
                },
            )
        else:
            wallet_id = self._cat_wallet(bytes(asset_id))
            if wallet_id is None:
                raise OptionsError("the wallet holds none of the quote asset to top up from")
            answer = self._rpc(
                "cat_spend",
                {
                    "wallet_id": wallet_id,
                    "amount": int(amount),
                    "inner_address": address,
                    "fee": int(fee),
                },
            )
        tx = answer.get("transaction_id") or (answer.get("transaction") or {}).get("name") or ""
        return str(tx)

    def transaction_confirmed(self, transaction_id: str) -> bool:
        """Whether the wallet's transaction has confirmed."""
        answer = self._rpc("get_transaction", {"transaction_id": str(transaction_id)})
        return bool((answer.get("transaction") or {}).get("confirmed"))

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
    if cfg.wallet.engine_fence:
        # [OPTIONS-FENCE] The book's own coin-selecting calls (its top-ups)
        # must not pick the sub-account's coins either: a top-up pays from
        # the rest of the wallet into the sub-account.
        from gui.services.options import fence

        client.set_fence(
            fence.WalletFence(client._post, cfg.wallet.address, cfg.address_prefix).coin_ids  # noqa: SLF001
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
    if not cfg.wallet.engine_fence:
        return engine_guard.EngineCheckedWallet(wallet, cfg.wallet.engine_metrics_url)
    from gui.services.options import fence

    rpc = getattr(wallet, "_rpc", None) or wallet_rpc(cfg)
    book_fence = fence.WalletFence(rpc, cfg.wallet.address, cfg.address_prefix)
    try:
        identity = book_fence.identity(cfg.network)
    except fence.FenceUnavailableError:
        identity = ""  # never matches: the book then signs nothing beside the engine
    return engine_guard.EngineCheckedWallet(
        wallet,
        cfg.wallet.engine_metrics_url,
        fenced=True,
        identity=identity,
        conflicts=book_fence.conflicting_offers,
    )


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
            # The wallet's own offers (the engine's, on a shared wallet) are
            # not liquidity the hedge could use: left out of the Dexie range.
            own_offers=wallet.open_offer_ids,
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
