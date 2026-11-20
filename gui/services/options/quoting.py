"""Which contracts to have on offer, and at what price.

Pure functions over the config, a market view and the contracts already on the
book. Nothing here signs, spends or talks to anything.

THE GRID. For every configured expiry there is one *slot* per configured
strike: calls at ``call_strikes_pct`` of spot and puts at ``put_strikes_pct``,
rounded to ``strike_step``. A slot is filled while an unsold contract of ours
sits in it. When that contract sells, the slot is empty again and a new one is
minted, at the strike the current spot implies, until a limit says stop. So the
book is always offering the same shape of market around wherever the price is.

A minted contract's strike can never change. When spot has drifted far enough
that an unsold contract no longer resembles its slot (``strike_drift_pct``), or
its expiry is close (``stop_before_expiry_seconds``), it is *retired*:
cancelled, so its collateral can back something current.

STEERING. Selling a call hands away upside on XCH we hold; selling a put takes
on downside. The net of the two over what has been sold is the book's delta, in
XCH. When it leans one way, the side that would lean it further is made dearer
and the side that would bring it back is made cheaper, by at most
``steer_strength`` at a lean of ``steer_scale_xch``. No ask is ever pushed
below its model value plus carry, nor below the configured minimum premium.
"""

from __future__ import annotations

import dataclasses
import math

from gui.services.options import pricing, store
from gui.services.options.builder import Asset, OptionTerms
from gui.services.options.config import UNLIMITED, UNSET, XCH_MOJOS, OptionsConfig
from gui.services.options.market import Market

CALL = "call"
PUT = "put"


@dataclasses.dataclass(frozen=True)
class Wanted:
    """A contract the grid wants minted."""

    slot: str
    side: str
    strike: float  # quote asset per XCH
    expiration: int
    underlying: Asset
    strike_asset: Asset

    def terms(self, creator_puzzle_hash: bytes) -> OptionTerms:
        return OptionTerms(
            underlying=self.underlying,
            strike=self.strike_asset,
            expiration_seconds=self.expiration,
            creator_puzzle_hash=creator_puzzle_hash,
        )


@dataclasses.dataclass(frozen=True)
class AskQuote:
    """How one ask was arrived at, kept beside the offer it priced.

    Per-XCH figures are in the quote asset, as :func:`pricing.ask_price` gives
    them; ``units`` is the ask itself, in quote-asset mojos for the whole
    contract. ``markup`` is all the ask adds to model value and carry before
    the floor: ``pricing.markup_fraction`` of the model value, whatever it
    took to reach ``pricing.min_premium``, and the steering applied to both.
    So ``per_xch`` is ``max(model + carry + markup, floor)``, and ``set_by``
    says which of the two it is: ``"model"`` when the steered sum came to more
    than the minimum premium, ``"floor"`` when the minimum held it up. On the
    first live run (2026-11-15) every ask was ``"floor"``, which nothing on
    record could show.
    """

    spot: float
    sigma: float  # the market's volatility; the model used it plus sigma_markup
    strike: float  # quote asset per XCH
    seconds_to_expiry: int
    model: float  # American value at sigma + sigma_markup
    carry: float  # return forgone on the locked collateral
    markup: float  # all that is added to model + carry before the floor, steered
    floor: float  # pricing.min_premium
    per_xch: float  # the ask per XCH
    units: int  # the ask for the whole contract, in quote-asset mojos
    set_by: str  # "model" or "floor"
    delta: float  # of one long option
    lean: float  # the book's lean, -1..1, that steered the markup

    def as_record(self) -> dict[str, float | int | str]:
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class Plan:
    mint: tuple[Wanted, ...]  # in priority order; empty when a limit blocks
    retire: tuple[bytes, ...]  # launcher ids of unsold contracts to cancel
    asks: dict[bytes, int]  # launcher id -> ask in quote-asset mojos, for unsold contracts
    blocked: str  # why nothing more is minted, or ""
    net_delta_xch: float
    # launcher id -> how its ask was worked out, for the record kept with each offer
    quotes: dict[bytes, AskQuote] = dataclasses.field(default_factory=dict)


def slot_name(side: str, percent: int, expiration: int) -> str:
    return f"{'C' if side == CALL else 'P'}{percent}@{expiration}"


def strike_for(spot: float, percent: int, step: float) -> float:
    """``percent`` of spot, rounded to the nearest ``step`` and never below one step."""
    steps = max(1, round(spot * percent / 100.0 / step))
    # Rounded again so 23 * 0.05 is 1.15, not 1.1500000000000001.
    return round(steps * step, 9)


def contract_size_xch(contract: store.Contract) -> float:
    """How many XCH a contract is over."""
    mojos = contract.underlying_amount if contract.side == CALL else contract.strike_amount
    return mojos / XCH_MOJOS


def contract_strike(contract: store.Contract, quote_unit_mojos: int) -> float:
    """A contract's strike in quote asset per XCH."""
    quote = contract.strike_amount if contract.side == CALL else contract.underlying_amount
    return (quote / quote_unit_mojos) / contract_size_xch(contract)


def _wanted(
    config: OptionsConfig, side: str, percent: int, expiration: int, spot: float
) -> Wanted | None:
    """The contract a slot should hold, or None if it is too small to strike.

    The quote leg is a whole number of the quote asset's mojos. A contract so
    small that its strike rounds to none of them cannot be written at all.
    """
    strike = strike_for(spot, percent, config.strike_step)
    size_xch = config.contract_xch_mojos / XCH_MOJOS
    quote_mojos = int(round(strike * size_xch * config.quote_unit_mojos))
    if quote_mojos < 1:
        return None
    # The strike the contract will really have, once its quote leg is a whole
    # number of mojos. For a small contract that is not the grid's strike, and
    # a target that kept the grid's would see its own contract as having
    # drifted, retire it, and mint the same one again, for ever.
    strike = quote_mojos / config.quote_unit_mojos / size_xch
    xch = Asset(None, config.contract_xch_mojos)
    quote = Asset(config.quote_asset_id, quote_mojos)
    return Wanted(
        slot=slot_name(side, percent, expiration),
        side=side,
        strike=strike,
        expiration=expiration,
        underlying=xch if side == CALL else quote,
        strike_asset=quote if side == CALL else xch,
    )


def net_delta_xch(
    contracts: list[store.Contract], market: Market, config: OptionsConfig, now: int
) -> float:
    """The XCH exposure the sold contracts add to (or take from) our holdings."""
    total = 0.0
    for contract in contracts:
        if contract.status != store.SOLD or contract.expiration <= now:
            continue
        years = (contract.expiration - now) / pricing.SECONDS_PER_YEAR
        option_delta = pricing.delta(
            market.spot,
            contract_strike(contract, config.quote_unit_mojos),
            years,
            config.pricing.rate,
            market.sigma,
            call=contract.side == CALL,
        )
        # We are short the option: a sold call (delta > 0) lowers our exposure,
        # a sold put (delta < 0) raises it.
        total -= option_delta * contract_size_xch(contract)
    return total


def _steer(side: str, lean: float, strength: float) -> float:
    """Multiplier on an ask's markup for a book leaning ``lean`` (-1..1) long."""
    # Long-leaning book: calls shed exposure (cheaper), puts add it (dearer).
    return 1.0 - strength * lean if side == CALL else 1.0 + strength * lean


def ask_mojos(
    contract: store.Contract, market: Market, config: OptionsConfig, now: int, lean: float
) -> int:
    """The ask for one unsold contract, in quote-asset mojos, rounded up."""
    return ask_quote(contract, market, config, now, lean).units


def ask_quote(
    contract: store.Contract, market: Market, config: OptionsConfig, now: int, lean: float
) -> AskQuote:
    """The ask for one unsold contract, with everything that went into it."""
    strike = contract_strike(contract, config.quote_unit_mojos)
    quote = pricing.ask_price(
        market.spot,
        strike,
        contract.expiration - now,
        call=contract.side == CALL,
        sigma=market.sigma,
        rate=config.pricing.rate,
        sigma_markup=config.pricing.sigma_markup,
        lock_rate=config.pricing.lock_rate,
        markup_fraction=config.pricing.markup_fraction,
        min_premium=config.pricing.min_premium,
    )
    # ``quote.markup`` includes whatever it took to reach the minimum premium,
    # so steering scales that too. The minimum is a floor, not a markup: it is
    # put back after steering.
    minimum = config.pricing.min_premium
    floor = max(quote.model + quote.carry, minimum)
    strength = min(max(config.pricing.steer_strength, 0.0), 0.9)
    factor = _steer(contract.side, lean, strength)
    markup = quote.markup * factor
    steered = quote.model + quote.carry + markup
    per_xch = max(floor, steered)
    # What set the ask really offered. Steering can lift an ask that needed the
    # minimum premium above it, or lower one the model set onto it. Unsteered,
    # an ask topped up to the minimum sums back to it within a rounding error.
    on_floor = steered < minimum or math.isclose(steered, minimum, rel_tol=1e-9)
    return AskQuote(
        spot=market.spot,
        sigma=market.sigma,
        strike=strike,
        seconds_to_expiry=contract.expiration - now,
        model=quote.model,
        carry=quote.carry,
        markup=markup,
        floor=minimum,
        per_xch=per_xch,
        units=quote_units(per_xch, contract_size_xch(contract), config.quote_unit_mojos),
        set_by="floor" if on_floor else "model",
        delta=quote.delta,
        lean=lean,
    )


def quote_units(per_xch: float, size_xch: float, unit_mojos: int) -> int:
    """``per_xch`` for a contract of ``size_xch``, in quote-asset mojos, rounded up.

    Rounded to 9 places first: an ask that sits exactly on ``min_premium`` comes
    back from the float sums as, say, 5.000000000000001 mojos, and rounding that
    up made it 6. With mojos of 0.001 wUSDC.b that was a 20% higher ask, which
    then flipped back and forth as the model drifted, re-quoting each time.
    """
    return max(1, math.ceil(round(per_xch * size_xch * unit_mojos, 9)))


def _over(cap: int, value: int) -> bool:
    return cap != UNLIMITED and value > cap


def plan(config: OptionsConfig, market: Market, contracts: list[store.Contract], now: int) -> Plan:
    """Decide what to mint, what to retire and what to ask."""
    active = [item for item in contracts if item.status in store.ACTIVE]
    unsold = [item for item in active if item.status in (store.MINTING, store.OPEN)]
    delta_xch = net_delta_xch(active, market, config, now)
    lean = max(-1.0, min(1.0, delta_xch / config.pricing.steer_scale_xch))

    targets: dict[str, Wanted] = {}
    too_small: set[str] = set()
    for expiration in config.expiries_at(now):
        if expiration - now <= config.stop_before_expiry_seconds:
            continue
        sides = [(CALL, percent) for percent in config.call_strikes_pct]
        sides += [(PUT, percent) for percent in config.put_strikes_pct]
        for side, percent in sides:
            wanted = _wanted(config, side, percent, expiration, market.spot)
            if wanted is None:
                too_small.add(slot_name(side, percent, expiration))
            else:
                targets[wanted.slot] = wanted

    retire: list[bytes] = []
    filled: set[str] = set()
    asks: dict[bytes, int] = {}
    quotes: dict[bytes, AskQuote] = {}
    for contract in unsold:
        target = targets.get(contract.slot)
        strike = contract_strike(contract, config.quote_unit_mojos)
        if target is None:
            # No longer in the grid: retire it. One whose slot merely cannot be
            # struck at today's contract size is left as it is.
            stale = contract.slot not in too_small
        else:
            stale = abs(strike - target.strike) / target.strike * 100.0 > config.strike_drift_pct
        if contract.status == store.OPEN and stale:
            retire.append(contract.launcher_id)
            continue
        filled.add(contract.slot)
        if contract.status == store.OPEN and contract.expiration > now:
            quoted = ask_quote(contract, market, config, now, lean)
            asks[contract.launcher_id] = quoted.units
            quotes[contract.launcher_id] = quoted

    limits = config.limits
    blocked = ""
    if UNSET in (
        limits.max_locked_xch,
        limits.max_locked_quote,
        limits.max_contracts,
        limits.max_contracts_per_expiry,
    ):
        blocked = "a limit under options.limits is not set; nothing will be minted"
    elif too_small:
        blocked = (
            "options.contract_xch is too small: at these strikes a contract comes to less than "
            "one mojo of the quote asset, so nothing will be minted"
        )

    locked_xch = sum(item.underlying_amount for item in active if item.side == CALL)
    locked_quote = sum(item.underlying_amount for item in active if item.side == PUT)
    per_expiry: dict[int, int] = {}
    for item in active:
        per_expiry[item.expiration] = per_expiry.get(item.expiration, 0) + 1
    count = len(active)

    mint: list[Wanted] = []
    if not blocked:
        # Nearest expiry first, and within it the strikes nearest the money,
        # which are the ones takers want most.
        ordered = sorted(
            (wanted for slot, wanted in targets.items() if slot not in filled),
            key=lambda wanted: (
                wanted.expiration,
                abs(wanted.strike - market.spot),
                wanted.side,
            ),
        )
        for wanted in ordered:
            if _over(limits.max_contracts, count + 1):
                blocked = "options.limits.max_contracts reached"
                break
            if _over(limits.max_contracts_per_expiry, per_expiry.get(wanted.expiration, 0) + 1):
                continue
            amount = wanted.underlying.amount
            if wanted.side == CALL:
                if _over(limits.max_locked_xch, locked_xch + amount):
                    continue
                locked_xch += amount
            else:
                if _over(limits.max_locked_quote, locked_quote + amount):
                    continue
                locked_quote += amount
            per_expiry[wanted.expiration] = per_expiry.get(wanted.expiration, 0) + 1
            count += 1
            mint.append(wanted)

    return Plan(
        mint=tuple(mint),
        retire=tuple(retire),
        asks=asks,
        blocked=blocked,
        net_delta_xch=delta_xch,
        quotes=quotes,
    )
