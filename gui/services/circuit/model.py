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


# Circuit's own API description (the openapi.json the probe saved on
# 2026-11-21) sets out the vaults, the treasury, the auctions, the governance
# coins and the announcers. Their fields are read as it has them: one it always
# gives must be there, and null only where it allows null. Anything else is an
# answer of an unexpected shape, so the older answer stays beside a note.


def _flag(record: Mapping[str, Any], key: str) -> bool:
    """A yes or no the API always gives: true or false.

    Left out, null, 1 or "true" is an answer of an unexpected shape, not a no:
    read as no, a vault in bad debt or open to liquidation would look sound.
    """
    value = record[key]
    if isinstance(value, bool):
        return value
    raise TypeError(f"expected {key} as true or false, got {value!r}")


def _maybe_flag(record: Mapping[str, Any], key: str) -> bool | None:
    """A yes or no the API may leave open (null): an auction's ``can_be_settled``."""
    value = record[key]
    if value is None or isinstance(value, bool):
        return value
    raise TypeError(f"expected {key} as true, false or null, got {value!r}")


def _text(record: Mapping[str, Any], key: str) -> str:
    """A name or a status: always text."""
    value = record[key]
    if isinstance(value, str):
        return value
    raise TypeError(f"expected {key} as text, got {value!r}")


def _optional_text(record: Mapping[str, Any], key: str) -> str | None:
    value = record.get(key)
    if value is None or isinstance(value, str):
        return value
    raise TypeError(f"expected {key} as text or null, got {value!r}")


# -- the sections -------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class PricePoint:
    usd: float
    at: int


def _price_pair(raw: Any) -> PricePoint:
    """A ``[price, unix time]`` pair, as every price in these answers comes."""
    if not isinstance(raw, list) or len(raw) != 2:
        raise TypeError(f"expected a [price, time] pair, got {raw!r}")
    price, at = raw
    return PricePoint(_number(price) / PRICE_SCALE, int(_time(at)))


def _point(raw: Any) -> PricePoint | None:
    """A price pair, or null: the oracle has none of that kind yet."""
    return None if raw is None else _price_pair(raw)


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
    # Every key is there in the live answer, a price left open as null. The
    # queue is a list, empty when no price waits: one left out or garbled
    # would have the page say there is "no newer price", so it is a note.
    delay = int(_time(answer["price_delay"]))
    matured = _point(answer["last_matured_price_info"])
    infos = answer["price_infos"]
    if not isinstance(infos, list):
        raise TypeError(f"expected price_infos as a list, got {infos!r}")
    queued = []
    for raw in infos:
        point = _price_pair(raw)
        if matured is None or point.at > matured.at:
            queued.append((point, point.at + delay))
    queued.sort(key=lambda item: item[0].at)
    return Oracle(_point(answer["statutes_price_info"]), matured, tuple(queued), delay)


@dataclasses.dataclass(frozen=True)
class Protocol:
    """Circuit's settings (its statutes) that a market maker cares about."""

    status: str
    #: As the answer gave it (0 or 1), so the page shows it as given.
    block_issuance: int | float
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
    def emergency(self) -> bool:
        """BLOCK_ISSUANCE is set: the protocol's core functions are off."""
        return self.block_issuance != 0


def protocol_from(answer: Any) -> Protocol:
    statutes = answer["implemented_statutes"]
    if not isinstance(statutes, Mapping):
        raise TypeError("implemented_statutes is not an object")
    order: list[tuple[int, str, Any]] = []
    # [[name, index], ...], each naming a statute; without them, the
    # statutes are listed in the order the answer gives them.
    labels = answer.get("statute_labels")
    if labels is not None and not isinstance(labels, list):
        raise TypeError(f"expected statute_labels as a list, got {labels!r}")
    if labels:
        for label in labels:
            if not isinstance(label, list) or len(label) != 2:
                raise TypeError(f"expected a statute label as [name, index], got {label!r}")
            name, index = label
            if not isinstance(name, str) or isinstance(index, bool) or not isinstance(index, int):
                raise TypeError(f"expected a statute label as [name, index], got {label!r}")
            order.append((index, name, statutes[name]))
        order.sort()
    else:
        order = [(i, name, value) for i, (name, value) in enumerate(statutes.items())]
    # The emergency switch and the status must both be there. An answer
    # without either says nothing of an emergency, and taken as the latest
    # answer it would hide one that an older answer showed: it is of an
    # unexpected shape, so the older answer stays beside a note.
    block_issuance = statutes[BLOCK_ISSUANCE]
    # Checked, but kept as given, so the page shows 0 or 1, not 0.0 or 1.0.
    # A string "0", false, NaN or a number no float holds is no state at all.
    _number(block_issuance)
    penalty = _optional_number(statutes.get("VAULT_LIQUIDATION_PENALTY_BPS"))
    usage = _optional_number(answer.get("current_collateral_usage"))
    cap = _optional_number(answer.get("max_collateral_cap"))
    counter = answer.get("price_update_counter")
    status = _text(answer, "status")  # "ok", or what is wrong
    return Protocol(
        status=status,
        block_issuance=block_issuance,
        # A factor that is there must be a number; a yearly rate too large
        # for a float is no rate, and the rest still shows.
        stability_fee_apy=apy_from_df(_optional_number(statutes.get("STABILITY_FEE_DF"))),
        savings_apy=apy_from_df(_optional_number(statutes.get("INTEREST_DF"))),
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


#: A vault's yes/no fields, all of which the API always gives.
VAULT_FLAGS = (
    "in_bad_debt",
    "in_liquidation",
    "is_biddable",
    "is_restartable",
    "is_startable",
    "is_liquidatable",
    "nearing_liquidation",
    "seized",
)


def _vault_flags(vault: Mapping[str, Any]) -> dict[str, bool]:
    """Every one of a vault's flags, each checked, even where an earlier one decides."""
    return {key: _flag(vault, key) for key in VAULT_FLAGS}


def _vault_state(flags: Mapping[str, bool]) -> str:
    if flags["in_bad_debt"]:
        return "bad debt"
    if flags["in_liquidation"]:
        return "auction running" + (", open to bids" if flags["is_biddable"] else "")
    if flags["is_restartable"]:
        return "auction can be restarted"
    if flags["is_startable"]:
        return "auction can be started"
    if flags["is_liquidatable"]:
        return "liquidatable"
    if flags["nearing_liquidation"]:
        return "nearing liquidation"
    if flags["seized"]:
        return "seized"
    return ""


def _vault_row(vault: Mapping[str, Any], flags: Mapping[str, bool], answered_at: float) -> VaultRow:
    debt = _number(vault["debt"]) / MOJOS_PER_CAT
    # A number, or "inf" for a vault with no debt (as all 214 were on 2026-11-21).
    raw_ratio = vault["collateral_ratio"]
    if isinstance(raw_ratio, str):
        if raw_ratio != "inf" or debt > 0:
            raise ValueError(
                f'expected collateral_ratio as a number, or "inf" with no debt, got {raw_ratio!r}'
            )
        ratio = None
    else:
        number = _number(raw_ratio)
        ratio = number if debt > 0 else None
    liquidation = _optional_number(vault["liquidation_price"])
    auction = _optional_number(vault["auction_price"])
    ttl = _optional_time(vault["auction_ttl"])
    return VaultRow(
        name=_text(vault, "name"),
        collateral_xch=_number(vault["collateral"]) / MOJOS_PER_XCH,
        debt_byc=debt,
        ratio=ratio,
        liquidation_usd=None if liquidation is None else liquidation / PRICE_SCALE,
        state=_vault_state(flags),
        auction_usd=None if auction is None else auction / PRICE_SCALE,
        auction_ends_at=None if ttl is None else answered_at + ttl,
        biddable=flags["is_biddable"],
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
        flags = _vault_flags(vault)
        row = _vault_row(vault, flags, answered_at)
        rows.append(row)
        if flags["in_bad_debt"]:
            bad_debt.append(row)
        elif flags["in_liquidation"]:
            in_auction.append(row)
        elif flags["is_liquidatable"] or flags["is_startable"] or flags["is_restartable"]:
            waiting.append(row)
        else:
            if flags["nearing_liquidation"]:
                nearing.append(row)
            # Listed once: a vault past liquidation is in one of the lists above.
            if row.debt_byc > 0 and row.liquidation_usd is not None and not flags["seized"]:
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
        is_ring=_flag(answer, "is_ring"),
        can_rebalance=_flag(answer, "can_rebalance"),
        can_start_recharge=_flag(answer, "can_start_recharge_auction"),
        can_start_surplus=_flag(answer, "can_start_surplus_auction"),
    )


@dataclasses.dataclass(frozen=True)
class AuctionRow:
    """A recharge auction (CRT sold for BYC) or a surplus auction (BYC sold for CRT)."""

    kind: str
    name: str
    status: str
    detail: str
    #: None where the API leaves it open (null).
    can_be_settled: bool | None


# Each row checks every field it reads, even where another decides what it
# shows: a garbled field is a note however the rest of the record reads.


def _recharge_row(auction: Mapping[str, Any], now: float) -> AuctionRow:
    bid = auction["last_bid"]
    params = auction["auction_params"]
    ends = None if params is None else _time(params["auction_expires_at"])
    if bid is not None:
        detail = (
            f"last bid {fmt_units(_number(bid['byc_bid_amount']) / MOJOS_PER_CAT)} BYC for "
            f"{fmt_units(_number(bid['crt_bid_amount']) / MOJOS_PER_CAT)} CRT; the bid stands "
            f"until {fmt_utc(_time(bid['bid_expires_at']), now)}"
        )
    elif ends is not None:
        detail = f"no bids yet; ends {fmt_utc(ends, now)}"
    else:
        detail = "not started"
    return AuctionRow(
        "Recharge",
        _text(auction, "name"),
        _text(auction, "status"),
        detail,
        _maybe_flag(auction, "can_be_settled"),
    )


def _surplus_row(auction: Mapping[str, Any], now: float) -> AuctionRow:
    lot = fmt_units(_number(auction["byc_lot_amount"]) / MOJOS_PER_CAT)
    best = fmt_units(_number(auction["crt_bid_amount"]) / MOJOS_PER_CAT)
    bid = auction["last_bid"]
    if bid is not None:
        detail = (
            f"{lot} BYC lot; best bid {best} CRT, standing until "
            f"{fmt_utc(_time(bid['bid_expires_at']), now)}"
        )
    else:
        detail = f"{lot} BYC lot; no bids yet"
    return AuctionRow(
        "Surplus",
        _text(auction, "name"),
        _text(auction, "status"),
        detail,
        _maybe_flag(auction, "can_be_settled"),
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
    # A governance coin's bill, status and last step may be left out (null).
    bill = coin.get("bill")
    if bill is None:
        proposal = "no bill"
    else:
        value = bill["value"]  # a statute's new value: text or a whole number
        if isinstance(value, bool) or not isinstance(value, (str, int)):
            raise TypeError(f"expected the bill's value as text or a whole number, got {value!r}")
        proposal = f"{_text(bill, 'statute_name')} = {value}"
    status = coin.get("status")
    operation = _optional_text(coin, "operation")
    if status is None:
        text = f"last step: {operation}" if operation else ""
    else:
        text = _text(status, "status")
        ends = _optional_time(status["status_expires_at"])
        if ends is not None:
            text += f" until {fmt_utc(ends, now)}"
    exitable = _optional_time(coin["exitable_at"])
    if exitable is None:
        exit_text = ""
    elif exitable <= now:
        exit_text = "can leave governance"
    else:
        exit_text = "can leave " + fmt_from_now(exitable, now)
    return BillRow(
        name=_text(coin, "name"),
        crt=_number(coin["amount"]) / MOJOS_PER_CAT,
        proposal=proposal,
        status=text,
        exit=exit_text,
    )


def _bill_spent(coin: Mapping[str, Any]) -> bool:
    """The API may leave a governance coin's ``spent`` out: then it is false."""
    return _flag(coin, "spent") if "spent" in coin else False


def bills_from(answer: Any, now: float) -> tuple[BillRow, ...]:
    if not isinstance(answer, list):
        raise TypeError("the governance list is not a list")
    # Every coin is checked, a spent one too, and then only those unspent are shown.
    rows = [(_bill_row(coin, now), _bill_spent(coin)) for coin in answer]
    return tuple(row for row, spent in rows if not spent)


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
    words.append("valid" if _flag(announcer, "valid") else "not valid")
    if not _flag(announcer, "approved"):
        words.append("not approved")
    if _flag(announcer, "penalizable"):
        words.append("can be penalised")
    if _flag(announcer, "in_cooldown"):
        words.append("in cooldown")
    expires_at = _time(announcer["expires_at"])
    expires = ("expires " if expires_at >= now else "expired ") + fmt_from_now(expires_at, now)
    return AnnouncerRow(
        name=_text(announcer, "launcher_id"),
        price_usd=_number(announcer["price"]) / PRICE_SCALE,
        state=", ".join(words),
        expires=expires,
        deposit_xch=_number(announcer["deposit"]) / MOJOS_PER_XCH,
    )


def announcers_from(answer: Any, now: float) -> tuple[AnnouncerRow, ...]:
    if not isinstance(answer, list):
        raise TypeError("the announcer list is not a list")
    # Every announcer is checked, a spent one too, and then only those unspent are shown.
    rows = [(_announcer_row(announcer, now), _flag(announcer, "spent")) for announcer in answer]
    return tuple(row for row, spent in rows if not spent)


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
