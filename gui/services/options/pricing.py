"""Option values: Black-Scholes, a Cox-Ross-Rubinstein tree, and the ask we quote.

Pure arithmetic, no I/O. Prices are per one unit of the underlying (per XCH),
in the quote asset (wUSDC.b); callers scale by the contract size.

Two models, for two jobs:

* :func:`black_scholes` is what xchoptions.com's "suggested price" is: plain
  European Black-Scholes on its recorded spot, 30-day hourly realized
  volatility and the 10-year Treasury rate. Reproducing it tells us what a
  retail maker who lists "at suggested" is asking.
* :func:`american` prices the contract as it really is, exercisable at any time.
  On the listed contracts the difference is under half a percent (calls on an
  asset with no yield are never worth exercising early), so the tree is there
  for correctness, not for edge.

:func:`ask_price` turns a model value into the price we are willing to sell at.
It adds the two costs the model ignores for a fully collateralised writer: the
return forgone on collateral that sits locked until expiry, and a markup.
"""

from __future__ import annotations

import dataclasses
import math

SECONDS_PER_YEAR = 365 * 24 * 3600


class PricingError(ValueError):
    """The inputs cannot be priced."""


def _check(spot: float, strike: float, years: float, sigma: float) -> None:
    for name, value in (("spot", spot), ("strike", strike)):
        if not math.isfinite(value) or value <= 0:
            raise PricingError(f"{name} must be a positive finite number")
    if not math.isfinite(years) or years < 0:
        raise PricingError("time to expiry must be a non-negative finite number")
    if not math.isfinite(sigma) or sigma < 0:
        raise PricingError("volatility must be a non-negative finite number")


def _norm_cdf(x: float) -> float:
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def intrinsic(spot: float, strike: float, *, call: bool) -> float:
    return max(0.0, spot - strike) if call else max(0.0, strike - spot)


def black_scholes(
    spot: float, strike: float, years: float, rate: float, sigma: float, *, call: bool
) -> float:
    """European option value per unit of underlying."""
    _check(spot, strike, years, sigma)
    if years == 0 or sigma == 0:
        forward_strike = strike * math.exp(-rate * years)
        return max(0.0, spot - forward_strike) if call else max(0.0, forward_strike - spot)
    root = sigma * math.sqrt(years)
    d1 = (math.log(spot / strike) + (rate + 0.5 * sigma * sigma) * years) / root
    d2 = d1 - root
    discounted = strike * math.exp(-rate * years)
    if call:
        return spot * _norm_cdf(d1) - discounted * _norm_cdf(d2)
    return discounted * _norm_cdf(-d2) - spot * _norm_cdf(-d1)


def delta(
    spot: float, strike: float, years: float, rate: float, sigma: float, *, call: bool
) -> float:
    """Black-Scholes delta of one long option: 0..1 for a call, -1..0 for a put."""
    _check(spot, strike, years, sigma)
    if years == 0 or sigma == 0:
        in_the_money = spot > strike if call else spot < strike
        return (1.0 if call else -1.0) if in_the_money else 0.0
    root = sigma * math.sqrt(years)
    d1 = (math.log(spot / strike) + (rate + 0.5 * sigma * sigma) * years) / root
    return _norm_cdf(d1) if call else _norm_cdf(d1) - 1.0


def american(
    spot: float,
    strike: float,
    years: float,
    rate: float,
    sigma: float,
    *,
    call: bool,
    steps: int = 200,
) -> float:
    """American option value per unit of underlying, on a CRR binomial tree."""
    _check(spot, strike, years, sigma)
    if steps < 1:
        raise PricingError("steps must be at least 1")
    if years == 0 or sigma == 0:
        return max(
            intrinsic(spot, strike, call=call),
            black_scholes(spot, strike, years, rate, sigma, call=call),
        )
    dt = years / steps
    up = math.exp(sigma * math.sqrt(dt))
    down = 1.0 / up
    growth = math.exp(rate * dt)
    p = (growth - down) / (up - down)
    if not 0.0 < p < 1.0:
        # The step is too coarse for this rate and volatility; the tree would
        # carry negative probabilities. Fall back rather than return nonsense.
        return max(
            intrinsic(spot, strike, call=call),
            black_scholes(spot, strike, years, rate, sigma, call=call),
        )
    discount = 1.0 / growth
    values = [
        intrinsic(spot * up**j * down ** (steps - j), strike, call=call) for j in range(steps + 1)
    ]
    for i in range(steps - 1, -1, -1):
        for j in range(i + 1):
            held = discount * (p * values[j + 1] + (1.0 - p) * values[j])
            exercised = intrinsic(spot * up**j * down ** (i - j), strike, call=call)
            values[j] = max(held, exercised)
    return values[0]


@dataclasses.dataclass(frozen=True)
class Quote:
    """The pieces of one ask, per unit of underlying, in the quote asset."""

    model: float  # American value at the ask volatility
    carry: float  # return forgone on the locked collateral
    markup: float
    delta: float  # of one long option

    @property
    def ask(self) -> float:
        return self.model + self.carry + self.markup


def ask_price(
    spot: float,
    strike: float,
    seconds_to_expiry: float,
    *,
    call: bool,
    sigma: float,
    rate: float = 0.0,
    sigma_markup: float = 0.0,
    lock_rate: float = 0.0,
    markup_fraction: float = 0.0,
    min_premium: float = 0.0,
) -> Quote:
    """The ask for one unit of underlying.

    ``sigma_markup`` is added to ``sigma`` (0.02 = two volatility points).
    ``lock_rate`` is the yearly return the locked collateral would otherwise
    earn; a call locks the underlying (worth ``spot``) and a put locks the
    strike. ``markup_fraction`` is a fraction of the model value. The ask is
    never below ``min_premium``, which keeps far out-of-the-money contracts from
    being listed for less than they cost to carry and settle.
    """
    for name, value in (
        ("sigma_markup", sigma_markup),
        ("lock_rate", lock_rate),
        ("markup_fraction", markup_fraction),
        ("min_premium", min_premium),
    ):
        if not math.isfinite(value) or value < 0:
            raise PricingError(f"{name} must be a non-negative finite number")
    if not math.isfinite(seconds_to_expiry) or seconds_to_expiry <= 0:
        raise PricingError("the option has expired")
    years = seconds_to_expiry / SECONDS_PER_YEAR
    ask_sigma = sigma + sigma_markup
    model = american(spot, strike, years, rate, ask_sigma, call=call)
    collateral = spot if call else strike
    carry = lock_rate * collateral * years
    markup = markup_fraction * model
    shortfall = max(0.0, min_premium - (model + carry + markup))
    return Quote(
        model=model,
        carry=carry,
        markup=markup + shortfall,
        delta=delta(spot, strike, years, rate, ask_sigma, call=call),
    )
