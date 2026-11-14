"""The pricer against the site's own numbers and against first principles."""

from __future__ import annotations

import math

import pytest

from gui.services.options import pricing

# Suggested prices xchoptions.com returned on 2026-11-14, with the inputs it
# reported: (side, size, strike total, expiry, read at, spot, sigma, rate, premium).
# Size and strike are in XCH and wUSDC.b for a call and the reverse for a put.
SITE = [
    ("call", 20, 22, 1796130000, 1794673016, 1.11, 0.8631988565847382, 0.050217716160617515, 1.76),
    ("put", 24, 20, 1798722000, 1794673016, 1.11, 0.8631988565847382, 0.050217716160617515, 3.726),
    ("put", 24, 20, 1796130000, 1794673016, 1.11, 0.8631988565847382, 0.050217716160617515, 2.716),
    ("call", 10, 15, 1796133600, 1794673016, 1.11, 0.8631988565847382, 0.050217716160617515, 0.054),
    ("put", 23, 20, 1796130000, 1794673016, 1.11, 0.8631988565847382, 0.050217716160617515, 2.068),
    ("call", 20, 26, 1798722000, 1794673016, 1.11, 0.8631988565847382, 0.050217716160617515, 1.478),
]


@pytest.mark.parametrize("row", SITE)
def test_black_scholes_reproduces_the_sites_suggested_price(row) -> None:
    side, size, strike_total, expiry, read_at, spot, sigma, rate, premium = row
    call = side == "call"
    xch = size if call else strike_total
    strike = (strike_total / size) if call else (size / strike_total)
    years = (expiry - read_at) / pricing.SECONDS_PER_YEAR

    value = pricing.black_scholes(spot, strike, years, rate, sigma, call=call) * xch

    # The site rounds to three decimals; the read time is known to the minute.
    assert value == pytest.approx(premium, abs=0.004)


def test_put_call_parity() -> None:
    spot, strike, years, rate, sigma = 1.11, 1.20, 0.2, 0.05, 0.86
    call = pricing.black_scholes(spot, strike, years, rate, sigma, call=True)
    put = pricing.black_scholes(spot, strike, years, rate, sigma, call=False)
    assert call - put == pytest.approx(spot - strike * math.exp(-rate * years), abs=1e-12)


def test_american_call_equals_european_and_put_is_worth_more() -> None:
    spot, strike, years, rate, sigma = 1.11, 1.30, 0.25, 0.05, 0.86
    european_call = pricing.black_scholes(spot, strike, years, rate, sigma, call=True)
    european_put = pricing.black_scholes(spot, strike, years, rate, sigma, call=False)

    american_call = pricing.american(spot, strike, years, rate, sigma, call=True, steps=400)
    american_put = pricing.american(spot, strike, years, rate, sigma, call=False, steps=400)

    # No yield on the underlying: early exercise of a call is never worth it.
    assert american_call == pytest.approx(european_call, rel=2e-3)
    assert american_put > european_put
    assert american_put == pytest.approx(european_put, rel=2e-2)


def test_american_value_is_never_below_intrinsic() -> None:
    for spot in (0.4, 0.9, 1.11, 2.5):
        for call in (True, False):
            value = pricing.american(spot, 1.2, 0.1, 0.05, 0.86, call=call)
            assert value >= pricing.intrinsic(spot, 1.2, call=call) - 1e-12


def test_at_expiry_the_value_is_intrinsic() -> None:
    assert pricing.black_scholes(1.5, 1.2, 0.0, 0.05, 0.86, call=True) == pytest.approx(0.3)
    assert pricing.black_scholes(1.5, 1.2, 0.0, 0.05, 0.86, call=False) == 0.0
    assert pricing.american(1.0, 1.2, 0.0, 0.05, 0.86, call=False) == pytest.approx(0.2)


def test_delta_has_the_right_sign_and_range() -> None:
    call = pricing.delta(1.11, 1.2, 0.1, 0.0, 0.86, call=True)
    put = pricing.delta(1.11, 1.2, 0.1, 0.0, 0.86, call=False)
    assert 0.0 < call < 1.0
    assert -1.0 < put < 0.0
    assert call - put == pytest.approx(1.0)
    assert pricing.delta(2.0, 1.0, 0.0, 0.0, 0.86, call=True) == 1.0
    assert pricing.delta(2.0, 1.0, 0.0, 0.0, 0.86, call=False) == 0.0


def test_ask_adds_carry_and_markup_to_the_model() -> None:
    seconds = 17 * 86400
    plain = pricing.ask_price(1.11, 1.2, seconds, call=True, sigma=0.86)
    loaded = pricing.ask_price(
        1.11, 1.2, seconds, call=True, sigma=0.86, lock_rate=0.20, markup_fraction=0.05
    )

    assert plain.carry == 0.0 and plain.markup == 0.0
    assert plain.ask == pytest.approx(plain.model)
    # A call locks the underlying: 20% a year on 1.11 for 17 days.
    assert loaded.carry == pytest.approx(0.20 * 1.11 * seconds / pricing.SECONDS_PER_YEAR)
    assert loaded.markup == pytest.approx(0.05 * loaded.model)
    assert loaded.ask == pytest.approx(loaded.model + loaded.carry + loaded.markup)


def test_a_put_carries_on_the_strike_it_locks() -> None:
    seconds = 30 * 86400
    quote = pricing.ask_price(1.11, 0.9, seconds, call=False, sigma=0.86, lock_rate=0.10)
    assert quote.carry == pytest.approx(0.10 * 0.9 * seconds / pricing.SECONDS_PER_YEAR)


def test_sigma_markup_raises_the_ask() -> None:
    seconds = 17 * 86400
    base = pricing.ask_price(1.11, 1.2, seconds, call=True, sigma=0.86)
    wider = pricing.ask_price(1.11, 1.2, seconds, call=True, sigma=0.86, sigma_markup=0.05)
    assert wider.ask > base.ask


def test_min_premium_floors_a_far_out_of_the_money_ask() -> None:
    quote = pricing.ask_price(1.11, 5.0, 86400, call=True, sigma=0.86, min_premium=0.01)
    assert quote.model < 0.001
    assert quote.ask == pytest.approx(0.01)


@pytest.mark.parametrize(
    "kwargs",
    [
        {"spot": 0.0},
        {"spot": float("nan")},
        {"strike": -1.0},
        {"sigma": -0.1},
        {"seconds_to_expiry": 0},
        {"seconds_to_expiry": -5},
        {"lock_rate": -0.1},
        {"markup_fraction": float("inf")},
    ],
)
def test_bad_inputs_are_refused(kwargs) -> None:
    args = {"spot": 1.11, "strike": 1.2, "seconds_to_expiry": 86400, "sigma": 0.86}
    args.update(kwargs)
    spot, strike, seconds = args.pop("spot"), args.pop("strike"), args.pop("seconds_to_expiry")
    with pytest.raises(pricing.PricingError):
        pricing.ask_price(spot, strike, seconds, call=True, **args)
