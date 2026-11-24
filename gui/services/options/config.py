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

SHARING IT ANYWAY. ``wallet.share_trading_wallet: true`` (off by default) lets
the book use the trading wallet's daemon and key on the understanding that the
engine is STOPPED for as long as the book has contracts open. It lifts only the
port and fingerprint checks, and adds four in their place: the book must name
the engine's own key, and the engine's key must be set (a step logs the wallet
in, and another key would switch it under the engine); it must name its
address (a blank one takes the wallet's current address, where the engine's
self-sends land); the wallet must be on this machine, where the engine check
can see; and every signature and push first checks that the engine is not
running (:mod:`engine_guard`), refusing if it is or if it cannot tell.

FUNDING FROM THE WALLET. ``wallet.fund_from_wallet: true`` (off by default)
lets what the options address is short of, in XCH or the quote asset, come
from the wallet's other addresses: collateral, fees, and the seed coins a
cancel starts from (one of the strike asset, one of XCH for the fee). What the
book owns still lands at the options address (all change, returned
collateral, premiums), and the funding coins' addresses count as ours in the
safety check, so it stays exact. ``live-test`` still funds itself from the
options address only.

THE ENGINE BESIDE OPEN CONTRACTS. ``wallet.engine_beside_open_contracts: true``
(off by default, read only with ``share_trading_wallet``) narrows "the engine
is STOPPED while contracts are open" to "the engine is stopped while the book
acts". Contracts that sit open do not hold the engine: their collateral is
locked in the contract, which the wallet does not spend, and their offers spend
only the option coin. What still holds it: the book being Live, a step in
progress, and a transaction the book has sent that could still confirm (the
wallet does not know those coins are being spent, so an engine offer could pick
them). A running engine can spend the loose coins at the options address, so
this needs ``fund_from_wallet``: the book then pays from the rest of the wallet
when it next acts. Every signature and push still checks that the engine is
stopped, as above.

THE ENGINE FENCE. ``wallet.engine_fence: true`` (off by default, needs
``share_trading_wallet``) makes the options address a sub-account the engine
leaves alone: the engine (v0.10.52 and later) sends that address's coins as
excluded_coin_ids on every wallet call that selects coins, and the app's own
spenders do the same (:mod:`fence`). The book may then be Live while the engine
runs, as long as the engine reports its fence on and fresh on its metrics
endpoint (:mod:`engine_guard`); otherwise the rules above apply. With the fence
the book spends only coins at its own address, so ``fund_from_wallet`` is
refused; ``target`` keeps the address topped up instead, by ordinary wallet
sends from the rest of the wallet.

``wallet.engine_fence: auto`` (engine v0.10.53 and later) is the same fence,
up only while the book is in use: switched on, a contract open, or a
transaction unsettled. The app keeps ``options_fence.flag`` beside the
engine's database for as long as that lasts and the engine follows it
(:func:`fence.set_flag`); with the book off and nothing open the flag goes,
and the options address's coins are trading funds again.

[DEFAULTS] ``auto`` is the default whenever the book shares the trading wallet
(``engine_fence: false`` turns it off); the engine reads the same default.
Under the fence ``fund_from_wallet`` and ``engine_beside_open_contracts`` are
not used, and are ignored with a warning rather than refused. Without an
``options.target`` the fence's target comes from the limits
(:func:`default_target`); ``target: false`` turns top-ups off.

[LADDER] ``contract_xch: auto`` (also when it is not written) is a ladder of
sizes offered side by side: 1, 10, 100, ... XCH (:data:`LADDER_XCH`). Every
strike and expiry gets a contract of each size, the smallest everywhere
first, as far as the limits allow: the whole 1 XCH grid, then 10 XCH
contracts, then 100 (:func:`gui.services.options.quoting.plan`). Small and
large buyers both find a contract at every strike and date. A written number
is that one size, as before. :func:`call_ladder` says what a ladder comes to.
"""

from __future__ import annotations

import calendar
import dataclasses
import datetime
import logging
import math
import os
from typing import Any

from gui.services.options import splash

log = logging.getLogger(__name__)

#: Warnings already given this process: a config is read on every step.
_WARNED: set[str] = set()


def _warn_once(message: str) -> None:
    if message not in _WARNED:
        _WARNED.add(message)
        log.warning("[options] %s", message)


XCH_MOJOS = 10**12

MAINNET = "mainnet"
TESTNET11 = "testnet11"

#: A cap that was never set. Refuses the action it bounds.
UNSET = -1
#: A cap explicitly set to ``unlimited``.
UNLIMITED = 0


class OptionsConfigError(ValueError):
    """The ``options:`` section is unusable. Nothing starts."""


#: Host names that mean the machine the book runs on.
_THIS_MACHINE = frozenset({"localhost", "127.0.0.1", "::1"})


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


def _mojos(units: float, scale: int, what: str) -> int:
    """``units`` in mojos, rounded to the nearest.

    [review #254] A finite number can still be too large to count in mojos
    (1e300): that is a config error here, not ``round``'s OverflowError,
    which no caller expects from a parser.
    """
    amount = units * scale
    if not math.isfinite(amount):
        raise OptionsConfigError(f"{what} is too large")
    return int(round(amount))


def _path(section: dict, key: str) -> str:
    """A file path, with ``~`` expanded as the chia section's paths are. "" if unset.

    The TLS libraries the paths go to do not expand it themselves.
    """
    raw = str(section.get(key) or "").strip()
    return os.path.expanduser(raw) if raw else ""


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
    # [review #254] A finite number can still be too large to count in mojos
    # (1e300 XCH): refused here, not left to math.floor's OverflowError.
    amount = value * scale
    if not math.isfinite(amount):
        raise OptionsConfigError(
            f"options.limits.{key} is too large; use 'unlimited' if no cap is intended"
        )
    # Floored: a cap must never round up past what the operator wrote.
    scaled = math.floor(amount)
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


def distinct_expiries(dates: Any) -> int:
    """[LADDER] How many expiries a written list of dates comes to, as :func:`parse` counts.

    The same moment written twice, or in two UTC offsets, is one expiry. Raises
    OptionsConfigError for an entry :func:`parse` would refuse.
    """
    return len({_expiry(item) for item in dates})


#: [DEFAULTS] With neither strike list written, the book writes both sides,
#: out of the money: calls above spot and puts below it. ``[]`` turns a side off.
DEFAULT_CALL_STRIKES_PCT: tuple[int, ...] = (110, 120)
DEFAULT_PUT_STRIKES_PCT: tuple[int, ...] = (90, 80)


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
    #: The operator's opt-in to signing from the trading engine's wallet while
    #: the engine is stopped. Off by default; see the module docstring.
    share_trading_wallet: bool = False
    #: Where a running engine answers (its Prometheus endpoint), asked before
    #: anything is signed while ``share_trading_wallet`` is on.
    engine_metrics_url: str = ""
    #: Fund collateral, fees and a cancel's seed coin from the rest of the
    #: wallet when the options address runs short. Off by default.
    fund_from_wallet: bool = False
    #: With ``share_trading_wallet``: open contracts do not hold the engine,
    #: only the book acting does. Off by default; needs ``fund_from_wallet``.
    engine_beside_open_contracts: bool = False
    #: With ``share_trading_wallet``: the engine fences the options address off
    #: and the book may be Live beside it. Off by default; refuses
    #: ``fund_from_wallet``. True for ``engine_fence: true`` and ``auto``.
    engine_fence: bool = False
    #: [AUTO-FENCE] ``engine_fence: auto``: the fence is up only while the book
    #: is in use (``fence.set_flag``), not for the engine's whole life.
    fence_follows_book: bool = False


@dataclasses.dataclass(frozen=True)
class TargetConfig:
    """What the options sub-account is kept at (``options.target``).

    Counted with the collateral its open contracts lock, so a mint does not
    look like a shortfall. Below ``1 - tolerance`` of a figure, a step tops
    the address up to the figure with an ordinary wallet send.
    """

    xch_mojos: int
    quote_mojos: int
    tolerance: float  # a fraction, 0.2 = 20%


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


#: [LADDER] The sizes ``options.contract_xch: auto`` offers, in whole XCH,
#: smallest first. Why 1 XCH at least: a contract's premium grows with its size
#: and its fees do not, and the quote asset's smallest unit (0.001 wUSDC.b) is
#: a coarse price step for a contract much under 1 XCH. The largest is a bound,
#: not a goal: the limits stop the ladder long before it.
LADDER_XCH: tuple[int, ...] = (1, 10, 100, 1_000, 10_000, 100_000, 1_000_000)
AUTO_CONTRACT_MIN_XCH = LADDER_XCH[0]
AUTO_CONTRACT_MAX_XCH = LADDER_XCH[-1]


def call_ladder(
    limits: Limits, call_strikes: int, expiries: int, put_strikes: int = 0
) -> tuple[tuple[int, int], ...]:
    """[LADDER] What ``contract_xch: auto`` comes to for calls, on an empty book.

    ``(XCH a contract, calls of that size)`` for each size at least one call
    reaches, smallest first, filled as :func:`gui.services.options.quoting.plan`
    fills them: every call strike and expiry at 1 XCH, then at 10 XCH, and so
    on, until ``max_locked_xch``, ``max_contracts`` or
    ``max_contracts_per_expiry`` stops it. Puts take their share of the two
    counts at each size before the next size starts; how much of the quote
    asset they lock depends on the price, so ``max_locked_quote`` is not
    counted here, and when it stops puts the calls may go further than this.

    Empty when no call can be minted: no call strikes or expiries, one of the
    three limits read here not set, or ``max_locked_xch`` under 1 XCH. With
    ``max_locked_xch`` unlimited calls stay at 1 XCH: there is nothing to size
    the ladder from.
    """
    if call_strikes <= 0 or expiries <= 0:
        return ()
    if UNSET in (limits.max_locked_xch, limits.max_contracts, limits.max_contracts_per_expiry):
        return ()
    held = 0
    on_expiry = [0] * expiries
    locked = 0
    out: list[tuple[int, int]] = []
    for size in LADDER_XCH:
        if out and limits.max_locked_xch == UNLIMITED:
            break
        calls = 0
        for index in range(expiries):
            room = call_strikes + put_strikes
            if limits.max_contracts_per_expiry != UNLIMITED:
                room = min(room, limits.max_contracts_per_expiry - on_expiry[index])
            if limits.max_contracts != UNLIMITED:
                room = min(room, limits.max_contracts - held)
            if room <= 0:
                continue
            here = min(call_strikes, room)
            if limits.max_locked_xch != UNLIMITED:
                here = min(here, (limits.max_locked_xch - locked) // (size * XCH_MOJOS))
            taken = here + min(put_strikes, room - here)
            calls += here
            locked += here * size * XCH_MOJOS
            on_expiry[index] += taken
            held += taken
        if calls <= 0:
            break
        out.append((size, calls))
    return tuple(out)


def _contract_size(section: dict) -> float | None:
    """``options.contract_xch`` in XCH; None for ``auto``, which not writing it also means.

    [LADDER] Left out or null is ``auto``, as an unwritten ``expiries`` is
    weekly and an unwritten ``target`` comes from the limits. Anything else
    must be a positive number: that one size, and no ladder.
    """
    raw = section.get("contract_xch")
    if raw is None or (isinstance(raw, str) and raw.strip().lower() == "auto"):
        return None
    if isinstance(raw, bool):
        raise OptionsConfigError("options.contract_xch must be a number of XCH, or auto")
    try:
        value = float(raw)
    except (TypeError, ValueError) as exc:
        raise OptionsConfigError(
            f"options.contract_xch must be a number of XCH, or auto (got {raw!r})"
        ) from exc
    # [review #254] Positive before it is scaled: -1e300 times a mojo count is
    # -inf, which round() answers with OverflowError.
    if not math.isfinite(value) or value <= 0:
        raise OptionsConfigError("options.contract_xch must be positive")
    if _mojos(value, XCH_MOJOS, "options.contract_xch") <= 0:  # under half a mojo
        raise OptionsConfigError("options.contract_xch must be positive")
    return value


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
    #: The node's own certificate authority (its config/ssl/ca/private_ca.crt).
    #: With it the node's certificate is checked; required for a node on
    #: another machine.
    ca_cert_path: str = ""

    @property
    def enabled(self) -> bool:
        return bool(self.cert_path)

    @property
    def local(self) -> bool:
        return self.host.strip().strip("[]").lower() in _THIS_MACHINE


#: [ROLLING] When a rolling expiry falls: the crypto options market's own time
#: (Deribit's), Fridays at 08:00 UTC. xchoptions.com has no standard dates.
EXPIRY_WEEKDAY = 4  # Friday
EXPIRY_HOUR_UTC = 8
_MAX_WEEKLY = 12
_MAX_MONTHLY = 6


def _expiry_moment(day: datetime.date) -> datetime.datetime:
    return datetime.datetime.combine(day, datetime.time(EXPIRY_HOUR_UTC), tzinfo=datetime.UTC)


def next_fridays(after: int, count: int) -> list[int]:
    """The next ``count`` Fridays at 08:00 UTC strictly after ``after`` (Unix seconds)."""
    start = datetime.datetime.fromtimestamp(after, tz=datetime.UTC).date()
    first = _expiry_moment(start + datetime.timedelta(days=(EXPIRY_WEEKDAY - start.weekday()) % 7))
    if first.timestamp() <= after:
        first += datetime.timedelta(weeks=1)
    return [int((first + datetime.timedelta(weeks=n)).timestamp()) for n in range(count)]


def month_end_fridays(after: int, count: int) -> list[int]:
    """The last Friday of the next ``count`` months, 08:00 UTC, strictly after ``after``."""
    moment = datetime.datetime.fromtimestamp(after, tz=datetime.UTC)
    year, month = moment.year, moment.month
    out: list[int] = []
    while len(out) < count:
        last = datetime.date(year, month, calendar.monthrange(year, month)[1])
        friday = _expiry_moment(
            last - datetime.timedelta(days=(last.weekday() - EXPIRY_WEEKDAY) % 7)
        )
        if friday.timestamp() > after:
            out.append(int(friday.timestamp()))
        month += 1
        if month > 12:
            year, month = year + 1, 1
    return out


@dataclasses.dataclass(frozen=True)
class ExpiryRule:
    """[ROLLING] ``options.expiries: weekly``: expiries that roll forward on their own.

    The next ``weekly`` Fridays at 08:00 UTC, plus the last Friday of the next
    ``monthly`` months, worked out afresh at every step. ``weekly`` alone is
    two weeklies; ``monthly`` alone is two month-ends.
    """

    weekly: int = 2
    monthly: int = 0

    def at(self, after: int) -> tuple[int, ...]:
        """The expiries strictly after ``after``, earliest first."""
        found = set(next_fridays(after, self.weekly)) | set(month_end_fridays(after, self.monthly))
        return tuple(sorted(found))


def _expiry_rule(raw: Any) -> ExpiryRule | None:
    """``options.expiries`` as a rule, or None when it is a list of dates (or ``none``).

    [DEFAULTS] Not written, or an empty list, is ``weekly``: a book with no
    expiries mints nothing and sits idle with nothing to say why. ``none``
    says so on purpose -- no new contracts, the book only looks after those
    it has (winding down).
    """
    if raw is None or (isinstance(raw, (list, tuple)) and not raw):
        return ExpiryRule(weekly=2, monthly=0)
    if isinstance(raw, str):
        word = raw.strip().lower()
        if word == "weekly":
            return ExpiryRule(weekly=2, monthly=0)
        if word == "monthly":
            return ExpiryRule(weekly=0, monthly=2)
        if word == "none":
            return None
        raise OptionsConfigError(
            f"options.expiries: {raw!r} is not a list of dates, 'weekly', 'monthly' or 'none'"
        )
    if not isinstance(raw, dict):
        return None
    unknown = set(raw) - {"weekly", "monthly"}
    if unknown:
        raise OptionsConfigError(
            f"options.expiries: unknown key(s) {sorted(unknown)}; use weekly and monthly"
        )
    counts = {}
    for key, cap in (("weekly", _MAX_WEEKLY), ("monthly", _MAX_MONTHLY)):
        value = raw.get(key, 0)
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= cap:
            raise OptionsConfigError(f"options.expiries.{key} must be a whole number, 0 to {cap}")
        counts[key] = value
    if counts["weekly"] + counts["monthly"] == 0:
        raise OptionsConfigError("options.expiries: weekly or monthly must be above 0")
    return ExpiryRule(**counts)


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
    #: ``options.target``: the sub-account's balance target. None = no top-ups.
    target: TargetConfig | None = None
    #: The app's CoinGecko key (``coingecko.api_key``, kept in secrets.yaml).
    #: Not an options setting; carried here so the price feed can send it.
    coingecko_api_key: str = dataclasses.field(default="", repr=False)
    #: [ROLLING] ``options.expiries: weekly`` (or a mapping): the expiries roll
    #: forward on their own and ``expiries`` is empty. None for a list of dates.
    expiry_rule: ExpiryRule | None = None
    #: [LADDER] ``options.contract_xch: auto`` (or not written): the sizes are
    #: the ladder, and ``contract_xch_mojos`` is its smallest.
    contract_auto: bool = False
    #: [LADDER] Every size on offer, in XCH mojos, smallest first. Empty means
    #: ``contract_xch_mojos`` alone; read it through :attr:`sizes_mojos`.
    contract_sizes_mojos: tuple[int, ...] = ()

    @property
    def address_prefix(self) -> str:
        return "xch" if self.network == MAINNET else "txch"

    @property
    def sizes_mojos(self) -> tuple[int, ...]:
        """[LADDER] The contract sizes on offer, in XCH mojos, smallest first."""
        return self.contract_sizes_mojos or (self.contract_xch_mojos,)

    def expiries_at(self, now: int) -> tuple[int, ...]:
        """The expiries the grid is built on at ``now``.

        A list of dates as written; a rule's, counted from the end of the stop
        window, so ``weekly`` keeps two weeklies quotable even the day before
        the nearer one stops being quoted.
        """
        if self.expiry_rule is None:
            return self.expiries
        return self.expiry_rule.at(now + self.stop_before_expiry_seconds)

    def quotable_expiries(self, now: int) -> tuple[int, ...]:
        """The expiries contracts are minted for at ``now``: those past the stop window."""
        return tuple(
            expiry
            for expiry in self.expiries_at(now)
            if expiry - now > self.stop_before_expiry_seconds
        )


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
    if _flag(wallet_raw, "share_trading_wallet", False):
        # [DEFAULTS] Sharing the trading wallet: its own connection (chia.*)
        # for whatever options.wallet does not write itself.
        trading = root.get("chia") if isinstance(root.get("chia"), dict) else {}
        wallet_raw = {
            "host": trading.get("wallet_host"),
            "port": trading.get("wallet_port") or 9256,
            "cert_path": trading.get("wallet_cert_path"),
            "key_path": trading.get("wallet_key_path"),
            "fingerprint": trading.get("wallet_fingerprint"),
            **{key: value for key, value in wallet_raw.items() if value not in (None, "")},
        }
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
    cert_path = _path(wallet_raw, "cert_path")
    key_path = _path(wallet_raw, "key_path")
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
    share = _flag(wallet_raw, "share_trading_wallet", False)
    trading_host = str(chia.get("wallet_host") or "localhost").strip().lower()
    host = str(wallet_raw.get("host") or "localhost").strip()
    ours, theirs = host.lower().strip("[]"), trading_host.strip("[]")
    same_host = ours == theirs or {ours, theirs} <= _THIS_MACHINE
    same_daemon = same_host and port == int(chia.get("wallet_port") or 9256)
    if same_daemon and not share:
        raise OptionsConfigError(
            "options.wallet points at the trading wallet's RPC port. A dedicated wallet is "
            "a second wallet daemon with its own key and port."
        )
    fingerprint_raw = wallet_raw.get("fingerprint")
    try:
        fingerprint = None if fingerprint_raw in (None, "") else int(fingerprint_raw)
    except (TypeError, ValueError) as exc:
        raise OptionsConfigError("options.wallet.fingerprint must be a number") from exc
    trading_key = str(chia.get("wallet_fingerprint") or "").strip()
    if fingerprint is not None and trading_key == str(fingerprint) and not share:
        raise OptionsConfigError(
            "options.wallet.fingerprint is the trading wallet's key (chia.wallet_fingerprint). "
            "A second daemon on the same key spends the same coins the engine's offers do; "
            "a dedicated wallet has a key of its own."
        )
    if share and not ({ours, theirs} <= _THIS_MACHINE):
        # The check that the engine is stopped looks at this machine: its
        # metrics port and its process list. A wallet, or an engine's view of
        # one, on another machine is out of its sight, and it would report an
        # engine there as not running.
        raise OptionsConfigError(
            "options.wallet.share_trading_wallet needs the wallet on this machine: "
            "options.wallet.host and chia.wallet_host must both be localhost, 127.0.0.1 or ::1. "
            "The check that the engine is stopped can only see this machine."
        )
    if share and (fingerprint is None or not trading_key or trading_key != str(fingerprint)):
        # Every step, dry runs included, logs the wallet in to
        # options.wallet.fingerprint, and with none set it uses whichever key
        # is open. On the engine's daemon either would switch the key under
        # the engine, and one daemon can be named by more than one host, so
        # this is required whatever the host and port say.
        raise OptionsConfigError(
            "options.wallet.share_trading_wallet: options.wallet.fingerprint is required and "
            "must be the engine's key, chia.wallet_fingerprint, which must be set too: a step "
            "logs the wallet in to it, and another key would be switched in under the engine"
        )
    address = str(wallet_raw.get("address") or "").strip()
    if share and not address:
        # A blank address takes the wallet's current one, which is the newest
        # it has derived: where the engine sends its own coins to itself, and
        # spends them from. Seen on mainnet: an engine self-send made a coin
        # there and the engine spent it minutes later.
        raise OptionsConfigError(
            "options.wallet.share_trading_wallet needs options.wallet.address set to an address "
            "of this wallet's key that the engine does not send to itself: a blank address takes "
            "the wallet's current address, which is where the engine's own self-sends land"
        )
    metrics_url = str(wallet_raw.get("engine_metrics_url") or "").strip()
    if share and not metrics_url:
        monitoring = root.get("monitoring") if isinstance(root.get("monitoring"), dict) else {}
        try:
            metrics_port = int(monitoring.get("prometheus_port") or 9090)
        except (TypeError, ValueError) as exc:
            raise OptionsConfigError(
                "monitoring.prometheus_port must be a number; or set "
                "options.wallet.engine_metrics_url"
            ) from exc
        metrics_url = f"http://localhost:{metrics_port}/metrics"
    fund = _flag(wallet_raw, "fund_from_wallet", False)
    beside = _flag(wallet_raw, "engine_beside_open_contracts", False)
    fence_raw = wallet_raw.get("engine_fence")
    if fence_raw is None:
        # [DEFAULTS] Sharing the trading wallet: the fence, following the book.
        follows = fence = share
    elif isinstance(fence_raw, str) and fence_raw.strip().lower() == "auto":
        # `auto` is the fence when the wallet is shared, and nothing on a
        # wallet of its own: the example config says it either way.
        follows = fence = share
    else:
        follows = False
        fence = _flag(wallet_raw, "engine_fence", False)
        if fence and not share:
            raise OptionsConfigError(
                "options.wallet.engine_fence needs options.wallet.share_trading_wallet: true; a "
                "wallet of its own needs no fence"
            )
    if fence and fund:
        # The fenced engine leaves only the options address alone. A book that
        # also took coins from the rest of the wallet would race the engine for
        # them again, as on 2026-11-15: the fence wins.
        _warn_once(
            "options.wallet.fund_from_wallet is not used with the engine fence: the book spends "
            "only coins at its own address, and options.target keeps it topped up"
        )
        fund = False
    if fence and beside:
        _warn_once(
            "options.wallet.engine_beside_open_contracts is not needed with the engine fence: the "
            "engine runs beside the book whatever it has open"
        )
        beside = False
    if share and beside and not fund:
        # A running engine's offers pick coins from the whole wallet, those at
        # the options address included. The book's next cancel or clawback
        # must not depend on them still being there.
        raise OptionsConfigError(
            "options.wallet.engine_beside_open_contracts needs options.wallet.fund_from_wallet: "
            "true. The engine may spend the coins at the options address while contracts are "
            "open, so the book must be able to pay from the rest of the wallet when it acts"
        )
    wallet = WalletConfig(
        mode=mode,
        host=host,
        port=port,
        cert_path=cert_path,
        key_path=key_path,
        fingerprint=fingerprint,
        address=address,
        share_trading_wallet=share,
        engine_metrics_url=metrics_url,
        fund_from_wallet=fund,
        engine_beside_open_contracts=beside,
        engine_fence=fence,
        fence_follows_book=follows,
    )

    quote_decimals = int(_number(section, "quote_asset_decimals", 3))
    if not 0 <= quote_decimals <= 12:
        raise OptionsConfigError("options.quote_asset_decimals must be between 0 and 12")
    # [LADDER] None for `auto`: the ladder, from 1 XCH up.
    written_xch = _contract_size(section)

    expiries_raw = section.get("expiries")
    expiry_rule = _expiry_rule(expiries_raw)
    if expiry_rule is not None:
        expiries: tuple[int, ...] = ()
    elif isinstance(expiries_raw, str):  # none: no new contracts
        expiries = ()
    else:
        if not isinstance(expiries_raw, (list, tuple)):
            raise OptionsConfigError(
                "options.expiries must be a list of dates, weekly, monthly, or a mapping of "
                "weekly and monthly counts"
            )
        expiries = tuple(sorted({_expiry(item) for item in expiries_raw}))
    if "call_strikes_pct" not in section and "put_strikes_pct" not in section:
        # [DEFAULTS] No strikes written: both sides. A side written alone keeps
        # the other off, as before, and so does a list written empty or null,
        # so an existing grid never gains puts or calls nobody chose.
        calls, puts = DEFAULT_CALL_STRIKES_PCT, DEFAULT_PUT_STRIKES_PCT
    else:
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
        cert_path=_path(node_raw, "cert_path"),
        key_path=_path(node_raw, "key_path"),
        ca_cert_path=_path(node_raw, "ca_cert_path"),
    )
    if bool(node.cert_path) != bool(node.key_path):
        raise OptionsConfigError("options.node needs both cert_path and key_path, or neither")
    if node.enabled and not node.local and not node.ca_cert_path:
        # The client certificate proves who we are to the node. It proves
        # nothing about the node, and the book settles transactions on what
        # the node says.
        raise OptionsConfigError(
            "options.node.ca_cert_path is required for a node on another machine: without it "
            "the node's certificate is not checked, and anyone between the two could answer "
            "in its place. It is that node's config/ssl/ca/private_ca.crt."
        )
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
    # [LADDER] What the hedge check and the target's headroom are sized by:
    # the one size written, or the ladder's smallest.
    contract_xch = float(AUTO_CONTRACT_MIN_XCH) if written_xch is None else written_xch
    contract_mojos = _mojos(contract_xch, XCH_MOJOS, "options.contract_xch")
    sizes = () if written_xch is not None else tuple(size * XCH_MOJOS for size in LADDER_XCH)
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
        expiry_rule=expiry_rule,
        contract_auto=written_xch is None,
        contract_sizes_mojos=sizes,
        target=_resolve_target(section, quote_unit, wallet, limits, contract_mojos, bool(puts)),
    )


def default_target(
    limits: Limits, contract_mojos: int, quote_unit: int, puts: bool
) -> TargetConfig | None:
    """[DEFAULTS] The fence's target when ``options.target`` is not set: the book's own limits.

    XCH: ``max_locked_xch`` plus one contract -- the most the grid may lock,
    with room for fees and the next mint. [LADDER] One contract of the
    smallest size: the larger ones are inside ``max_locked_xch`` already.
    Quote: two units, a cancel's seed coin, plus ``max_locked_quote`` when
    puts are written. None when ``max_locked_xch`` is not a number: nothing
    moves into the options address without a cap the operator chose.
    """
    if limits.max_locked_xch <= 0:  # UNSET or UNLIMITED
        return None
    quote = 2 * quote_unit
    if puts and limits.max_locked_quote > 0:
        quote += limits.max_locked_quote
    xch = limits.max_locked_xch + contract_mojos
    # [LADDER] Topped up once it is short by half a contract of the smallest
    # size, or by 20%, whichever comes first. A sold call that is exercised
    # takes its collateral with it; with the ladder's larger sizes the target is
    # large, and 20% of it is more than a refill can do without -- the address
    # held too little to mint the refill and too much to be topped up. Short by
    # less than half a contract, it still holds every mint the limits allow,
    # with half a contract to spare for fees.
    tolerance = min(0.2, contract_mojos / 2 / xch)
    return TargetConfig(xch_mojos=xch, quote_mojos=quote, tolerance=tolerance)


def _resolve_target(
    section: dict,
    quote_unit: int,
    wallet: WalletConfig,
    limits: Limits,
    contract_mojos: int,
    puts: bool,
) -> TargetConfig | None:
    """``options.target`` as written; with the fence and none written, :func:`default_target`."""
    raw = section.get("target")
    if raw is False:
        return None  # top-ups off
    if raw is None:
        if not wallet.engine_fence:
            return None
        return default_target(limits, contract_mojos, quote_unit, puts)
    return _target(section, quote_unit)


def _target(section: dict, quote_unit: int) -> TargetConfig | None:
    """``options.target``: ``xch`` and ``quote`` in whole units, ``tolerance_pct``."""
    raw = section.get("target")
    if raw is None:
        return None
    if not isinstance(raw, dict):
        raise OptionsConfigError("options.target must be a mapping (xch, quote, tolerance_pct)")
    xch = _number(raw, "xch", 0.0)
    quote = _number(raw, "quote", 0.0)
    tolerance = _number(raw, "tolerance_pct", 20.0)
    if tolerance >= 100.0:
        raise OptionsConfigError("options.target.tolerance_pct must be below 100")
    return TargetConfig(
        xch_mojos=_mojos(xch, XCH_MOJOS, "options.target.xch"),
        quote_mojos=_mojos(quote, quote_unit, "options.target.quote"),
        tolerance=tolerance / 100.0,
    )


def resolve_database_path(
    cfg: OptionsConfig, config_path: str | os.PathLike[str], *, cwd: str | None = None
) -> OptionsConfig:
    """``database_path`` made absolute, read beside config.yaml. One rule for every entry point.

    The command line used to read a relative path against the folder it was
    run from, and the app's Options page and its engine check read it beside
    config.yaml: run from another folder, the two looked at different books,
    and the check could pass an engine start over open contracts. Both read it
    beside config.yaml now. A book found only where the old rule put it is not
    silently replaced by a new, empty one: that is refused, saying where it is.
    """
    if cfg.database_path == ":memory:" or os.path.isabs(cfg.database_path):
        return cfg
    beside = os.path.abspath(
        os.path.join(os.path.dirname(os.path.abspath(config_path)), cfg.database_path)
    )
    here = os.path.abspath(os.path.join(cwd or os.getcwd(), cfg.database_path))
    if here != beside and os.path.exists(here) and not os.path.exists(beside):
        raise OptionsConfigError(
            f"options.database_path {cfg.database_path!r} is read beside config.yaml ({beside}), "
            f"but the book is at {here}, where it was read against the folder the command ran "
            "from. Move it beside config.yaml, or set options.database_path to its absolute path."
        )
    return dataclasses.replace(cfg, database_path=beside)
