"""What the Circuit page shows, worked out from Circuit's API answers.

Plain Python, no Qt, so it is tested against the answers the probe saved from
the live server on 2026-11-21 (``tests/fixtures``). :func:`build_view` takes
the answers as the server sent them and returns display values. An answer of
an unexpected shape -- Circuit v2 is coming -- leaves its section empty with
the reason in :attr:`CircuitView.problems`; nothing here raises on one.

Units, as Circuit v1 answers (checked against the 2026-11-21 answers):

* XCH amounts are in mojos, 10**12 to the XCH. BYC and CRT are CATs, 1,000
  mojos to the unit.
* Prices are US dollars per XCH times 100 (94 is $0.94), stamped
  ``[price, unix time]``. The protocol counts a BYC as $1, so a vault's
  liquidation price and a liquidation auction's price are in the same units.
* ``collateral_ratio`` is a plain ratio (1.91 is 191%), or the string
  ``"inf"`` for a vault with no debt.
* The stability fee and the savings rate are per-minute discount factors
  scaled by 10**10 (10000000801 is about 4.30% a year).
"""

from __future__ import annotations

import dataclasses
import datetime as _dt
import math
from collections.abc import Mapping
from typing import Any

MOJOS_PER_XCH = 10**12
MOJOS_PER_CAT = 1000
PRICE_SCALE = 100
DF_SCALE = 10**10
MINUTES_PER_YEAR = 525_600
#: How many of the vaults nearest liquidation the page lists.
CLOSEST_VAULTS = 8

#: The statute that switches the protocol's core functions off (Circuit's
#: governance guide): anything but 0 means Circuit is in an emergency.
BLOCK_ISSUANCE = "BLOCK_ISSUANCE"


# -- formatting ---------------------------------------------------------------


def fmt_units(value: float) -> str:
    """An amount of XCH, BYC or CRT: whole units from 1,000 up, else up to 3 decimals."""
    if abs(value) >= 1000:
        return f"{value:,.0f}"
    text = f"{value:,.3f}".rstrip("0").rstrip(".")
    return "0" if text in ("", "-0") else text


def fmt_usd(value: float) -> str:
    return f"${value:,.2f}"


def fmt_pct(fraction: float, decimals: int = 0) -> str:
    """0.043 is "4%" (or "4.30%" with 2 decimals)."""
    return f"{fraction * 100:,.{decimals}f}%"


def fmt_duration(seconds: float) -> str:
    """A span for "in ..." or "... ago": 45 s, 21 min, 3 h 5 min, 4 days."""
    seconds = abs(seconds)
    if seconds < 90:
        return f"{seconds:.0f} s"
    minutes = seconds / 60
    if minutes < 90:
        return f"{minutes:.0f} min"
    hours = int(minutes // 60)
    if hours < 48:
        rest = int(minutes - hours * 60)
        return f"{hours} h {rest} min" if rest else f"{hours} h"
    return f"{hours // 24} days"


def fmt_from_now(at: float, now: float) -> str:
    """ "in 21 min" or "3 min ago"."""
    if at >= now:
        return "in " + fmt_duration(at - now)
    return fmt_duration(now - at) + " ago"


def fmt_utc(at: float, now: float) -> str:
    """A time in UTC; the date too unless it is within a day of ``now``.

    A time no calendar can hold, from a garbled answer, is shown as its number.
    """
    try:
        when = _dt.datetime.fromtimestamp(at, _dt.timezone.utc)  # noqa: UP017 -- 3.10, as api.py
    except (OverflowError, OSError, ValueError):
        return f"unix time {at:.0f}" if isinstance(at, float) else f"unix time {at}"
    if abs(at - now) < 86_400:
        return when.strftime("%H:%M UTC")
    return when.strftime("%Y-%m-%d %H:%M UTC")


def short_id(name: str) -> str:
    """The start and end of a coin or launcher id: 58b72f09...5bf1."""
    text = str(name)
    return text if len(text) <= 14 else f"{text[:8]}...{text[-4:]}"


def apy_from_df(discount_factor: Any) -> float | None:
    """The yearly rate of a per-minute discount factor scaled by 10**10.

    None for anything but a positive number, and for a factor whose yearly
    rate no float can hold (10**12, say, is 100 times a minute).
    """
    if isinstance(discount_factor, bool) or not isinstance(discount_factor, (int, float)):
        return None
    try:
        per_minute = float(discount_factor) / DF_SCALE
        if not (per_minute > 0 and math.isfinite(per_minute)):
            return None
        return math.exp(MINUTES_PER_YEAR * math.log(per_minute)) - 1
    except OverflowError:
        return None


def _number(value: Any) -> float:
    """A finite JSON number; ``TypeError`` for anything else (a bool included).

    Python's JSON parser takes ``NaN`` and ``Infinity``, and an integer too big
    for a float raises ``OverflowError``: both are answers of an unexpected
    shape, not figures to show.
    """
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError(f"expected a number, got {value!r}")
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"expected a finite number, got {value!r}")
    return number


def _optional_number(value: Any) -> float | None:
    return None if value is None else _number(value)


#: The last second of 9999, the end of Python's calendar: a time or a span in
#: an answer past it (or below 0) is not one, and would break the formatters.
MAX_TIME = 253_402_300_799


def _time(value: Any) -> float:
    """A Unix time, or a span in seconds, from an answer; ``ValueError`` outside 0..MAX_TIME."""
    number = _number(value)
    if not 0 <= number <= MAX_TIME:
        raise ValueError(f"expected a time, got {value!r}")
    return number


def _optional_time(value: Any) -> float | None:
    return None if value is None else _time(value)


def _flag(value: Any) -> bool:
    return value is True


# -- the sections -------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class PricePoint:
    usd: float
    at: int


def _point(raw: Any) -> PricePoint | None:
    if raw is None:
        return None
    price, at = raw
    return PricePoint(_number(price) / PRICE_SCALE, int(_time(at)))


@dataclasses.dataclass(frozen=True)
class Oracle:
    """The price the protocol measures vaults against, and what comes next."""

    #: The statutes' price: what collateral ratios and liquidations use.
    protocol_price: PricePoint | None
    #: The oracle's newest price old enough to be used.
    matured: PricePoint | None
    #: Newer prices, each with the time it can be used.
    queued: tuple[tuple[PricePoint, int], ...]
    price_delay_s: int


def oracle_from(answer: Any) -> Oracle:
    delay = int(_time(answer["price_delay"]))
    matured = _point(answer.get("last_matured_price_info"))
    queued = []
    for raw in answer.get("price_infos") or ():
        point = _point(raw)
        if point is not None and (matured is None or point.at > matured.at):
            queued.append((point, point.at + delay))
    queued.sort(key=lambda item: item[0].at)
    return Oracle(_point(answer.get("statutes_price_info")), matured, tuple(queued), delay)


@dataclasses.dataclass(frozen=True)
class Protocol:
    """Circuit's settings (its statutes) that a market maker cares about."""

    status: str
    block_issuance: Any
    stability_fee_apy: float | None
    savings_apy: float | None
    liquidation_ratio_pct: float | None
    liquidation_penalty_pct: float | None
    collateral_locked_xch: float | None
    collateral_cap_xch: float | None
    price_updates: int | None
    #: Every statute, in the protocol's order: (index, name, value).
    statutes: tuple[tuple[int, str, Any], ...]

    @property
    def emergency(self) -> bool | None:
        """BLOCK_ISSUANCE is set: the protocol's core functions are off.

        None when the answer has no BLOCK_ISSUANCE: the state is not known.
        """
        if self.block_issuance is None:
            return None
        return self.block_issuance != 0


def protocol_from(answer: Any) -> Protocol:
    statutes = answer["implemented_statutes"]
    if not isinstance(statutes, Mapping):
        raise TypeError("implemented_statutes is not an object")
    order: list[tuple[int, str, Any]] = []
    labels = answer.get("statute_labels")
    if labels:
        for name, index in labels:
            order.append((int(_number(index)), str(name), statutes.get(name)))
        order.sort()
    else:
        order = [(i, str(name), value) for i, (name, value) in enumerate(statutes.items())]
    penalty = _optional_number(statutes.get("VAULT_LIQUIDATION_PENALTY_BPS"))
    usage = _optional_number(answer.get("current_collateral_usage"))
    cap = _optional_number(answer.get("max_collateral_cap"))
    counter = answer.get("price_update_counter")
    return Protocol(
        status=str(answer.get("status", "")),
        block_issuance=statutes.get(BLOCK_ISSUANCE),
        stability_fee_apy=apy_from_df(statutes.get("STABILITY_FEE_DF")),
        savings_apy=apy_from_df(statutes.get("INTEREST_DF")),
        liquidation_ratio_pct=_optional_number(statutes.get("VAULT_LIQUIDATION_RATIO_PCT")),
        liquidation_penalty_pct=None if penalty is None else penalty / 100,
        collateral_locked_xch=None if usage is None else usage / MOJOS_PER_XCH,
        collateral_cap_xch=None if cap is None else cap / MOJOS_PER_XCH,
        price_updates=None if counter is None else int(_number(counter)),
        statutes=tuple(order),
    )


@dataclasses.dataclass(frozen=True)
class VaultRow:
    name: str
    collateral_xch: float
    debt_byc: float
    #: Collateral value over debt (1.91 is 191%); None with no debt.
    ratio: float | None
    liquidation_usd: float | None
    state: str
    #: A running liquidation auction's price, when it ends, and whether it takes bids.
    auction_usd: float | None = None
    auction_ends_at: float | None = None
    biddable: bool = False


def _vault_state(vault: Mapping[str, Any]) -> str:
    if _flag(vault.get("in_bad_debt")):
        return "bad debt"
    if _flag(vault.get("in_liquidation")):
        return "auction running" + (", open to bids" if _flag(vault.get("is_biddable")) else "")
    if _flag(vault.get("is_restartable")):
        return "auction can be restarted"
    if _flag(vault.get("is_startable")):
        return "auction can be started"
    if _flag(vault.get("is_liquidatable")):
        return "liquidatable"
    if _flag(vault.get("nearing_liquidation")):
        return "nearing liquidation"
    if _flag(vault.get("seized")):
        return "seized"
    return ""


def _vault_row(vault: Mapping[str, Any], answered_at: float) -> VaultRow:
    debt = _number(vault["debt"]) / MOJOS_PER_CAT
    raw_ratio = vault.get("collateral_ratio")
    ratio = None
    if debt > 0 and raw_ratio is not None and not isinstance(raw_ratio, str):
        ratio = _number(raw_ratio)
    liquidation = _optional_number(vault.get("liquidation_price"))
    auction = _optional_number(vault.get("auction_price"))
    ttl = _optional_time(vault.get("auction_ttl"))
    return VaultRow(
        name=str(vault["name"]),
        collateral_xch=_number(vault["collateral"]) / MOJOS_PER_XCH,
        debt_byc=debt,
        ratio=ratio,
        liquidation_usd=None if liquidation is None else liquidation / PRICE_SCALE,
        state=_vault_state(vault),
        auction_usd=None if auction is None else auction / PRICE_SCALE,
        auction_ends_at=None if ttl is None else answered_at + ttl,
        biddable=_flag(vault.get("is_biddable")),
    )


@dataclasses.dataclass(frozen=True)
class Vaults:
    count: int
    with_debt: int
    collateral_xch: float
    debt_byc: float
    #: Liquidation auctions running now.
    in_auction: tuple[VaultRow, ...]
    #: Liquidatable vaults nobody has started (or restarted) an auction for.
    waiting: tuple[VaultRow, ...]
    nearing: tuple[VaultRow, ...]
    bad_debt: tuple[VaultRow, ...]
    #: The other vaults with debt (none of the above), the highest liquidation price first.
    closest: tuple[VaultRow, ...]

    @property
    def bad_debt_byc(self) -> float:
        return sum(row.debt_byc for row in self.bad_debt)


def vaults_from(answer: Any, answered_at: float) -> Vaults:
    if not isinstance(answer, list):
        raise TypeError("the vault list is not a list")
    rows = []
    in_auction, waiting, nearing, bad_debt, open_debt = [], [], [], [], []
    for vault in answer:
        row = _vault_row(vault, answered_at)
        rows.append(row)
        if _flag(vault.get("in_bad_debt")):
            bad_debt.append(row)
        elif _flag(vault.get("in_liquidation")):
            in_auction.append(row)
        elif any(
            _flag(vault.get(key)) for key in ("is_liquidatable", "is_startable", "is_restartable")
        ):
            waiting.append(row)
        else:
            if _flag(vault.get("nearing_liquidation")):
                nearing.append(row)
            # Listed once: a vault past liquidation is in one of the lists above.
            if (
                row.debt_byc > 0
                and row.liquidation_usd is not None
                and not _flag(vault.get("seized"))
            ):
                open_debt.append(row)
    open_debt.sort(key=lambda row: row.liquidation_usd or 0.0, reverse=True)
    return Vaults(
        count=len(rows),
        with_debt=sum(1 for row in rows if row.debt_byc > 0),
        collateral_xch=sum(row.collateral_xch for row in rows),
        debt_byc=sum(row.debt_byc for row in rows),
        in_auction=tuple(in_auction),
        waiting=tuple(waiting),
        nearing=tuple(nearing),
        bad_debt=tuple(bad_debt),
        closest=tuple(open_debt[:CLOSEST_VAULTS]),
    )


@dataclasses.dataclass(frozen=True)
class Treasury:
    balance_byc: float
    minimum_byc: float
    maximum_byc: float
    coins: int
    is_ring: bool
    can_rebalance: bool
    can_start_recharge: bool
    can_start_surplus: bool


def treasury_from(answer: Any) -> Treasury:
    return Treasury(
        balance_byc=_number(answer["treasury_balance"]) / MOJOS_PER_CAT,
        minimum_byc=_number(answer["treasury_minimum"]) / MOJOS_PER_CAT,
        maximum_byc=_number(answer["treasury_maximum"]) / MOJOS_PER_CAT,
        coins=int(_number(answer["num_treasury_coins"])),
        is_ring=_flag(answer.get("is_ring")),
        can_rebalance=_flag(answer.get("can_rebalance")),
        can_start_recharge=_flag(answer.get("can_start_recharge_auction")),
        can_start_surplus=_flag(answer.get("can_start_surplus_auction")),
    )


@dataclasses.dataclass(frozen=True)
class AuctionRow:
    """A recharge auction (CRT sold for BYC) or a surplus auction (BYC sold for CRT)."""

    kind: str
    name: str
    status: str
    detail: str
    can_be_settled: bool


def _recharge_row(auction: Mapping[str, Any], now: float) -> AuctionRow:
    bid = auction.get("last_bid")
    params = auction.get("auction_params")
    if bid:
        detail = (
            f"last bid {fmt_units(_number(bid['byc_bid_amount']) / MOJOS_PER_CAT)} BYC for "
            f"{fmt_units(_number(bid['crt_bid_amount']) / MOJOS_PER_CAT)} CRT; the bid stands "
            f"until {fmt_utc(_time(bid['bid_expires_at']), now)}"
        )
    elif params:
        detail = f"no bids yet; ends {fmt_utc(_time(params['auction_expires_at']), now)}"
    else:
        detail = "not started"
    return AuctionRow(
        "Recharge",
        str(auction["name"]),
        str(auction.get("status") or ""),
        detail,
        _flag(auction.get("can_be_settled")),
    )


def _surplus_row(auction: Mapping[str, Any], now: float) -> AuctionRow:
    lot = fmt_units(_number(auction["byc_lot_amount"]) / MOJOS_PER_CAT)
    bid = auction.get("last_bid")
    if bid:
        best = fmt_units(_number(auction["crt_bid_amount"]) / MOJOS_PER_CAT)
        detail = (
            f"{lot} BYC lot; best bid {best} CRT, standing until "
            f"{fmt_utc(_time(bid['bid_expires_at']), now)}"
        )
    else:
        detail = f"{lot} BYC lot; no bids yet"
    return AuctionRow(
        "Surplus",
        str(auction["name"]),
        str(auction.get("status") or ""),
        detail,
        _flag(auction.get("can_be_settled")),
    )


def auctions_from(answer: Any, kind: str, now: float) -> tuple[AuctionRow, ...]:
    if not isinstance(answer, list):
        raise TypeError(f"the {kind} auction list is not a list")
    make = _recharge_row if kind == "recharge" else _surplus_row
    return tuple(make(auction, now) for auction in answer)


@dataclasses.dataclass(frozen=True)
class BillRow:
    """A governance coin: CRT locked to propose (or veto) a statute change."""

    name: str
    crt: float
    proposal: str
    status: str
    exit: str


def _bill_row(coin: Mapping[str, Any], now: float) -> BillRow:
    bill = coin.get("bill")
    if bill:
        proposal = f"{bill['statute_name']} = {bill['value']}"
    else:
        proposal = "no bill"
    status = coin.get("status")
    if status:
        text = str(status["status"])
        ends = status.get("status_expires_at")
        if ends is not None:
            text += f" until {fmt_utc(_time(ends), now)}"
    else:
        operation = coin.get("operation")
        text = f"last step: {operation}" if operation else ""
    exitable = coin.get("exitable_at")
    if exitable is None:
        exit_text = ""
    elif _time(exitable) <= now:
        exit_text = "can leave governance"
    else:
        exit_text = "can leave " + fmt_from_now(_time(exitable), now)
    return BillRow(
        name=str(coin["name"]),
        crt=_number(coin["amount"]) / MOJOS_PER_CAT,
        proposal=proposal,
        status=text,
        exit=exit_text,
    )


def bills_from(answer: Any, now: float) -> tuple[BillRow, ...]:
    if not isinstance(answer, list):
        raise TypeError("the governance list is not a list")
    return tuple(_bill_row(coin, now) for coin in answer if not _flag(coin.get("spent")))


@dataclasses.dataclass(frozen=True)
class AnnouncerRow:
    """One of the oracle's price announcers."""

    name: str
    price_usd: float
    state: str
    expires: str
    deposit_xch: float


def _announcer_row(announcer: Mapping[str, Any], now: float) -> AnnouncerRow:
    words = []
    words.append("valid" if _flag(announcer.get("valid")) else "not valid")
    if not _flag(announcer.get("approved")):
        words.append("not approved")
    if _flag(announcer.get("penalizable")):
        words.append("can be penalised")
    if _flag(announcer.get("in_cooldown")):
        words.append("in cooldown")
    expires_at = _time(announcer["expires_at"])
    expires = ("expires " if expires_at >= now else "expired ") + fmt_from_now(expires_at, now)
    return AnnouncerRow(
        name=str(announcer["launcher_id"]),
        price_usd=_number(announcer["price"]) / PRICE_SCALE,
        state=", ".join(words),
        expires=expires,
        deposit_xch=_number(announcer["deposit"]) / MOJOS_PER_XCH,
    )


def announcers_from(answer: Any, now: float) -> tuple[AnnouncerRow, ...]:
    if not isinstance(answer, list):
        raise TypeError("the announcer list is not a list")
    return tuple(
        _announcer_row(announcer, now) for announcer in answer if not _flag(announcer.get("spent"))
    )


# -- the whole page -----------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class CircuitView:
    oracle: Oracle | None = None
    protocol: Protocol | None = None
    vaults: Vaults | None = None
    treasury: Treasury | None = None
    recharge_auctions: tuple[AuctionRow, ...] | None = None
    surplus_auctions: tuple[AuctionRow, ...] | None = None
    bills: tuple[BillRow, ...] | None = None
    announcers: tuple[AnnouncerRow, ...] | None = None
    #: Why a read's section is empty, by read name.
    problems: Mapping[str, str] = dataclasses.field(default_factory=dict)

    @property
    def price_usd(self) -> float | None:
        """The protocol's XCH price (the statutes'), which vaults are measured against."""
        if self.oracle is not None and self.oracle.protocol_price is not None:
            return self.oracle.protocol_price.usd
        return None

    def collateral_ratio(self) -> float | None:
        """All collateral's value over all debt, at the protocol's price."""
        price = self.price_usd
        if price is None or self.vaults is None or self.vaults.debt_byc <= 0:
            return None
        return self.vaults.collateral_xch * price / self.vaults.debt_byc

    def drop_to_liquidation(self, row: VaultRow) -> float | None:
        """How far XCH must fall from the protocol's price for ``row`` to be liquidated."""
        price = self.price_usd
        if not price or row.liquidation_usd is None:
            return None
        return 1 - row.liquidation_usd / price


#: Read name -> the field of :class:`CircuitView` its answer fills.
_SECTIONS = {
    "oracle": "oracle",
    "statutes": "protocol",
    "vaults": "vaults",
    "treasury": "treasury",
    "recharge_auctions": "recharge_auctions",
    "surplus_auctions": "surplus_auctions",
    "bills": "bills",
    "announcers": "announcers",
}

#: What building a section raises on an answer of an unexpected shape.
SHAPE_ERRORS: tuple[type[Exception], ...] = (
    AttributeError,
    KeyError,
    IndexError,
    TypeError,
    ValueError,
    ArithmeticError,  # OverflowError, ZeroDivisionError
)


def shape_problem(exc: BaseException) -> str:
    """The note a section shows for an answer of an unexpected shape."""
    return f"the answer has an unexpected shape ({type(exc).__name__}: {exc})"


def section_from(read: str, answer: Any, *, now: float, answered_at: float | None = None) -> Any:
    """One read's section, built from its answer.

    Raises one of :data:`SHAPE_ERRORS` for an answer of an unexpected shape,
    so the page can check an answer before it replaces the last good one.
    ``answered_at`` is when it came (default ``now``).
    """
    if read == "oracle":
        return oracle_from(answer)
    if read == "statutes":
        return protocol_from(answer)
    if read == "vaults":
        return vaults_from(answer, now if answered_at is None else answered_at)
    if read == "treasury":
        return treasury_from(answer)
    if read == "recharge_auctions":
        return auctions_from(answer, "recharge", now)
    if read == "surplus_auctions":
        return auctions_from(answer, "surplus", now)
    if read == "bills":
        return bills_from(answer, now)
    if read == "announcers":
        return announcers_from(answer, now)
    raise KeyError(f"no section is built from the read {read!r}")


def build_view(
    answers: Mapping[str, Any],
    *,
    now: float,
    errors: Mapping[str, str] | None = None,
    answered_at: Mapping[str, float] | None = None,
) -> CircuitView:
    """The page's contents from the answers so far, by read name.

    ``errors`` says why a read has no answer; ``answered_at`` when each
    answer came (relative times in an answer, such as a liquidation
    auction's time left, count from it; default ``now``).
    """
    errors = errors or {}
    answered_at = answered_at or {}
    built: dict[str, Any] = {}
    problems: dict[str, str] = {}
    for read, field in _SECTIONS.items():
        if read not in answers:
            if read in errors:
                problems[read] = errors[read]
            continue
        try:
            value = section_from(read, answers[read], now=now, answered_at=answered_at.get(read))
        except SHAPE_ERRORS as exc:
            problems[read] = shape_problem(exc)
            continue
        built[field] = value
        if read in errors:
            # An older answer is shown, but the latest read failed: say so.
            problems[read] = errors[read]
    return CircuitView(problems=problems, **built)
