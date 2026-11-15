"""The ``options:`` section of config.yaml, parsed and checked.

No ``options:`` section, or ``enabled: false``, means the subsystem is off.
``dry_run`` defaults to true: bundles are built and checked but never signed,
broadcast or uploaded.

CAPS FAIL CLOSED. The limits that bound how much can be locked have no default.
A limit that was never set refuses to mint, as ``warp.max_unwrap_usdc`` does; it
is not read as "unlimited". Write ``unlimited`` to mean that.

TRADING WALLET. ``wallet.mode: trading`` is parsed and refused. Options can be
written from the wallet the engine trades through only once the engine excludes
option coins from its own offers and books option events; until then a mint
would look like an unexplained outflow and could trip the drawdown breaker.
"""

from __future__ import annotations

import dataclasses
import datetime
import math
from typing import Any

from gui.services.options import splash

XCH_MOJOS = 10**12

MAINNET = "mainnet"
TESTNET11 = "testnet11"

#: A cap that was never set. Refuses the action it bounds.
UNSET = -1
#: A cap explicitly set to ``unlimited``.
UNLIMITED = 0


class OptionsConfigError(ValueError):
    """The ``options:`` section is unusable. Nothing starts."""


def _number(section: dict, key: str, default: float | None, *, minimum: float = 0.0) -> float:
    raw = section.get(key, default)
    if raw is None:
        raise OptionsConfigError(f"options: {key} is required")
    if isinstance(raw, bool):
        raise OptionsConfigError(f"options: {key} must be a number")
    try:
        value = float(raw)
    except (TypeError, ValueError) as exc:
        raise OptionsConfigError(f"options: {key} must be a number (got {raw!r})") from exc
    if not math.isfinite(value) or value < minimum:
        raise OptionsConfigError(f"options: {key} must be a finite number >= {minimum}")
    return value


def _flag(section: dict, key: str, default: bool) -> bool:
    """A yes/no setting. Only a YAML true or false will do.

    ``bool("false")`` is True, so a quoted "false", or "no", or "off", would
    read as yes: for ``enabled`` that is a book switched on by a typo.
    """
    raw = section.get(key)
    if raw is None:
        return default
    if isinstance(raw, bool):
        return raw
    raise OptionsConfigError(f"options: {key} must be true or false (got {raw!r})")


def _cap(section: dict, key: str, scale: int) -> int:
    """A limit in mojos or units: UNSET if absent, UNLIMITED if so stated."""
    raw = section.get(key)
    if raw is None or str(raw).strip() == "":
        return UNSET
    if str(raw).strip().lower() == "unlimited":
        return UNLIMITED
    if isinstance(raw, bool):
        raise OptionsConfigError(f"options.limits.{key} must be a number or 'unlimited'")
    try:
        value = float(raw)
    except (TypeError, ValueError) as exc:
        raise OptionsConfigError(
            f"options.limits.{key} must be a number or 'unlimited' (got {raw!r})"
        ) from exc
    if not math.isfinite(value) or value <= 0:
        raise OptionsConfigError(
            f"options.limits.{key} must be positive; use 'unlimited' if no cap is intended"
        )
    # Floored: a cap must never round up past what the operator wrote.
    scaled = math.floor(value * scale)
    if scaled <= 0:
        raise OptionsConfigError(f"options.limits.{key} is below one unit")
    return scaled


def _asset_id(raw: Any, key: str) -> bytes:
    text = str(raw or "").strip().lower()
    if text.startswith("0x"):
        text = text[2:]
    try:
        value = bytes.fromhex(text)
    except ValueError as exc:
        raise OptionsConfigError(f"options: {key} must be 64 hex characters") from exc
    if len(value) != 32:
        raise OptionsConfigError(f"options: {key} must be 64 hex characters")
    return value


def _expiry(raw: Any) -> int:
    if isinstance(raw, bool):
        raise OptionsConfigError("options.expiries: entries must be timestamps")
    if isinstance(raw, (int, float)):
        return int(raw)
    if isinstance(raw, datetime.datetime):
        moment = raw
    else:
        try:
            moment = datetime.datetime.fromisoformat(str(raw).strip().replace("Z", "+00:00"))
        except ValueError as exc:
            raise OptionsConfigError(f"options.expiries: {raw!r} is not an ISO-8601 time") from exc
    if moment.tzinfo is None:
        raise OptionsConfigError(
            f"options.expiries: {raw!r} has no time zone; write e.g. 2026-12-01T13:00:00Z"
        )
    return int(moment.timestamp())


def _percents(raw: Any, key: str) -> tuple[int, ...]:
    if raw is None:
        return ()
    if not isinstance(raw, (list, tuple)):
        raise OptionsConfigError(f"options: {key} must be a list of percentages")
    values = []
    for item in raw:
        if isinstance(item, bool) or not isinstance(item, (int, float)) or not 1 <= item <= 1000:
            raise OptionsConfigError(f"options: {key} entries must be between 1 and 1000")
        values.append(int(item))
    if len(set(values)) != len(values):
        raise OptionsConfigError(f"options: {key} has a repeated entry")
    return tuple(values)


@dataclasses.dataclass(frozen=True)
class WalletConfig:
    mode: str
    host: str
    port: int
    cert_path: str
    key_path: str
    fingerprint: int | None
    address: str  # the one address used as creator, owner and change; "" = ask the wallet


@dataclasses.dataclass(frozen=True)
class PricingConfig:
    rate: float
    sigma_markup: float
    lock_rate: float
    markup_fraction: float
    min_premium: float  # per XCH, in the quote asset
    steer_strength: float
    steer_scale_xch: float


@dataclasses.dataclass(frozen=True)
class Limits:
    max_locked_xch: int  # mojos
    max_locked_quote: int  # quote-asset mojos
    max_contracts: int
    max_contracts_per_expiry: int
    max_fee_mojos: int


@dataclasses.dataclass(frozen=True)
class MarketConfig:
    max_age_seconds: float
    max_spread_pct: float
    require_book: bool
    hedge_size_xch: float


@dataclasses.dataclass(frozen=True)
class SplashConfig:
    """A Splash! daemon each new offer is also handed to. Off unless enabled."""

    enabled: bool = False
    submit_url: str = splash.DEFAULT_SUBMIT_URL


@dataclasses.dataclass(frozen=True)
class NodeConfig:
    """The operator's own full node, read instead of coinset when it is given."""

    host: str
    port: int
    cert_path: str
    key_path: str

    @property
    def enabled(self) -> bool:
        return bool(self.cert_path)


@dataclasses.dataclass(frozen=True)
class OptionsConfig:
    enabled: bool
    dry_run: bool
    network: str
    wallet: WalletConfig
    quote_asset_id: bytes
    quote_unit_mojos: int
    contract_xch_mojos: int
    expiries: tuple[int, ...]
    call_strikes_pct: tuple[int, ...]
    put_strikes_pct: tuple[int, ...]
    strike_step: float
    strike_drift_pct: float
    offer_ttl_seconds: int
    refresh_before_seconds: int
    reprice_pct: float
    withdraw_pct: float
    stop_before_expiry_seconds: int
    confirm_timeout_seconds: int
    fee_mojos: int  # per transaction; 0 with fee_auto
    fee_auto: bool  # size each fee from the wallet's estimate, under limits.max_fee_mojos
    withdraw_min_life_seconds: int
    withdraw_cooldown_seconds: int
    max_withdrawals_per_hour: int
    upload_offers: bool
    pricing: PricingConfig
    limits: Limits
    market: MarketConfig
    node: NodeConfig
    coinset_url: str
    chain_poll_seconds: int
    chain_refresh_seconds: int
    database_path: str
    splash: SplashConfig = dataclasses.field(default_factory=SplashConfig)
    #: The app's CoinGecko key (``coingecko.api_key``, kept in secrets.yaml).
    #: Not an options setting; carried here so the price feed can send it.
    coingecko_api_key: str = dataclasses.field(default="", repr=False)

    @property
    def address_prefix(self) -> str:
        return "xch" if self.network == MAINNET else "txch"


def disabled() -> OptionsConfig | None:
    return None


def parse(root: Any) -> OptionsConfig | None:
    """Parse the whole config mapping. Returns None when options are off."""
    if not isinstance(root, dict):
        return None
    section = root.get("options")
    if section is None:
        return None
    if not isinstance(section, dict):
        raise OptionsConfigError("options: must be a mapping")
    if not _flag(section, "enabled", False):
        return None

    network = str(section.get("network", MAINNET)).strip().lower()
    if network not in (MAINNET, TESTNET11):
        raise OptionsConfigError("options.network must be mainnet or testnet11")

    wallet_raw = section.get("wallet") or {}
    if not isinstance(wallet_raw, dict):
        raise OptionsConfigError("options.wallet must be a mapping")
    mode = str(wallet_raw.get("mode", "dedicated")).strip().lower()
    if mode == "trading":
        raise OptionsConfigError(
            "options.wallet.mode: trading is not supported yet. The engine does not exclude "
            "option coins from its own offers or book option events, so a mint from the "
            "trading wallet would collide with resting offers and read as a loss. Use a "
            "dedicated wallet (mode: dedicated)."
        )
    if mode != "dedicated":
        raise OptionsConfigError("options.wallet.mode must be dedicated")
    cert_path = str(wallet_raw.get("cert_path") or "").strip()
    key_path = str(wallet_raw.get("key_path") or "").strip()
    if not cert_path or not key_path:
        raise OptionsConfigError(
            "options.wallet.cert_path and key_path are required: the dedicated wallet's "
            "private_wallet.crt and private_wallet.key"
        )
    try:
        port = int(wallet_raw.get("port") or 0)
    except (TypeError, ValueError) as exc:
        raise OptionsConfigError("options.wallet.port must be a number") from exc
    if not 1 <= port <= 65535:
        raise OptionsConfigError(
            "options.wallet.port is required: the dedicated wallet's RPC port, which must "
            "differ from the trading wallet's"
        )
    chia = root.get("chia") if isinstance(root.get("chia"), dict) else {}
    coingecko = root.get("coingecko") if isinstance(root.get("coingecko"), dict) else {}
    trading_host = str(chia.get("wallet_host") or "localhost").strip().lower()
    host = str(wallet_raw.get("host") or "localhost").strip()
    local = {"localhost", "127.0.0.1", "::1"}
    same_host = host.lower() == trading_host or {host.lower(), trading_host} <= local
    if same_host and port == int(chia.get("wallet_port") or 9256):
        raise OptionsConfigError(
            "options.wallet points at the trading wallet's RPC port. A dedicated wallet is "
            "a second wallet daemon with its own key and port."
        )
    fingerprint_raw = wallet_raw.get("fingerprint")
    try:
        fingerprint = None if fingerprint_raw in (None, "") else int(fingerprint_raw)
    except (TypeError, ValueError) as exc:
        raise OptionsConfigError("options.wallet.fingerprint must be a number") from exc
    wallet = WalletConfig(
        mode=mode,
        host=host,
        port=port,
        cert_path=cert_path,
        key_path=key_path,
        fingerprint=fingerprint,
        address=str(wallet_raw.get("address") or "").strip(),
    )

    quote_decimals = int(_number(section, "quote_asset_decimals", 3))
    if not 0 <= quote_decimals <= 12:
        raise OptionsConfigError("options.quote_asset_decimals must be between 0 and 12")
    contract_xch = _number(section, "contract_xch", None, minimum=0.0)
    contract_mojos = int(round(contract_xch * XCH_MOJOS))
    if contract_mojos <= 0:
        raise OptionsConfigError("options.contract_xch must be positive")

    expiries_raw = section.get("expiries") or []
    if not isinstance(expiries_raw, (list, tuple)):
        raise OptionsConfigError("options.expiries must be a list")
    expiries = tuple(sorted({_expiry(item) for item in expiries_raw}))
    calls = _percents(section.get("call_strikes_pct"), "call_strikes_pct")
    puts = _percents(section.get("put_strikes_pct"), "put_strikes_pct")

    strike_step = _number(section, "strike_step", 0.05)
    if strike_step <= 0:
        raise OptionsConfigError("options.strike_step must be positive")
    ttl = int(_number(section, "offer_ttl_seconds", 900, minimum=60))
    refresh = int(_number(section, "refresh_before_seconds", 180))
    if refresh >= ttl:
        raise OptionsConfigError(
            "options.refresh_before_seconds must be shorter than offer_ttl_seconds"
        )

    pricing_raw = section.get("pricing") or {}
    limits_raw = section.get("limits") or {}
    market_raw = section.get("market") or {}
    node_raw = section.get("node") or {}
    splash_raw = section.get("splash") or {}
    for name, value in (
        ("pricing", pricing_raw),
        ("limits", limits_raw),
        ("market", market_raw),
        ("node", node_raw),
        ("splash", splash_raw),
    ):
        if not isinstance(value, dict):
            raise OptionsConfigError(f"options.{name} must be a mapping")

    try:
        splash_config = SplashConfig(
            enabled=_flag(splash_raw, "enabled", False),
            submit_url=splash.check_submit_url(
                splash_raw.get("submit_url") or splash.DEFAULT_SUBMIT_URL
            ),
        )
    except ValueError as exc:
        raise OptionsConfigError(f"options.splash.submit_url is {exc}") from exc

    node = NodeConfig(
        host=str(node_raw.get("host") or "localhost").strip(),
        port=int(_number(node_raw, "port", 8555, minimum=1)),
        cert_path=str(node_raw.get("cert_path") or "").strip(),
        key_path=str(node_raw.get("key_path") or "").strip(),
    )
    if bool(node.cert_path) != bool(node.key_path):
        raise OptionsConfigError("options.node needs both cert_path and key_path, or neither")
    chain_poll = int(_number(section, "chain_poll_seconds", 30))
    chain_refresh = int(_number(section, "chain_refresh_seconds", 3600))
    if chain_refresh < chain_poll:
        raise OptionsConfigError(
            "options.chain_refresh_seconds must not be shorter than chain_poll_seconds"
        )

    quote_unit = 10**quote_decimals
    limits = Limits(
        max_locked_xch=_cap(limits_raw, "max_locked_xch", XCH_MOJOS),
        max_locked_quote=_cap(limits_raw, "max_locked_quote", quote_unit),
        max_contracts=_cap(limits_raw, "max_contracts", 1),
        max_contracts_per_expiry=_cap(limits_raw, "max_contracts_per_expiry", 1),
        max_fee_mojos=int(_number(limits_raw, "max_fee_mojos", 0)),
    )
    fee_auto = str(section.get("fee_mojos", 0)).strip().lower() == "auto"
    fee = 0 if fee_auto else int(_number(section, "fee_mojos", 0))
    if fee > limits.max_fee_mojos:
        raise OptionsConfigError("options.fee_mojos is above options.limits.max_fee_mojos")

    return OptionsConfig(
        enabled=True,
        dry_run=_flag(section, "dry_run", True),
        network=network,
        wallet=wallet,
        quote_asset_id=_asset_id(section.get("quote_asset_id"), "quote_asset_id"),
        quote_unit_mojos=quote_unit,
        contract_xch_mojos=contract_mojos,
        expiries=expiries,
        call_strikes_pct=calls,
        put_strikes_pct=puts,
        strike_step=strike_step,
        strike_drift_pct=_number(section, "strike_drift_pct", 25.0),
        offer_ttl_seconds=ttl,
        refresh_before_seconds=refresh,
        reprice_pct=_number(section, "reprice_pct", 2.0),
        withdraw_pct=_number(section, "withdraw_pct", 2.0, minimum=0.01),
        withdraw_min_life_seconds=int(_number(section, "withdraw_min_life_seconds", 180)),
        withdraw_cooldown_seconds=int(_number(section, "withdraw_cooldown_seconds", 300)),
        max_withdrawals_per_hour=int(_number(section, "max_withdrawals_per_hour", 6)),
        stop_before_expiry_seconds=int(_number(section, "stop_before_expiry_seconds", 6 * 3600)),
        confirm_timeout_seconds=int(_number(section, "confirm_timeout_seconds", 1800, minimum=60)),
        fee_mojos=fee,
        fee_auto=fee_auto,
        upload_offers=_flag(section, "upload_offers", True),
        pricing=PricingConfig(
            rate=_number(pricing_raw, "rate", 0.0),
            sigma_markup=_number(pricing_raw, "sigma_markup", 0.02),
            lock_rate=_number(pricing_raw, "lock_rate", 0.10),
            markup_fraction=_number(pricing_raw, "markup_fraction", 0.03),
            min_premium=_number(pricing_raw, "min_premium", 0.01),
            steer_strength=_number(pricing_raw, "steer_strength", 0.25),
            steer_scale_xch=_number(pricing_raw, "steer_scale_xch", 100.0, minimum=1e-9),
        ),
        limits=limits,
        market=MarketConfig(
            max_age_seconds=_number(market_raw, "max_age_seconds", 600, minimum=1),
            max_spread_pct=_number(market_raw, "max_spread_pct", 10.0, minimum=0.01),
            require_book=_flag(market_raw, "require_book", True),
            hedge_size_xch=_number(market_raw, "hedge_size_xch", contract_xch, minimum=1e-9),
        ),
        node=node,
        coinset_url=str(section.get("coinset_url") or "").strip(),
        chain_poll_seconds=chain_poll,
        chain_refresh_seconds=chain_refresh,
        database_path=str(section.get("database_path") or "data/options_jobs.db").strip(),
        splash=splash_config,
        coingecko_api_key=str(coingecko.get("api_key") or "").strip(),
    )
