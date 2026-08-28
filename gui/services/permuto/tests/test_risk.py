"""Inventory and margin policy. Pure -- no sockets, no clock."""

from __future__ import annotations

import math

import pytest

from gui.services.permuto.orders import OrderIntent, Side, depth_credit_usd
from gui.services.permuto.risk import (
    CARRIED_IM_MULTIPLIER,
    FLATTEN_MARGIN_UTILISATION,
    MAX_MARGIN_UTILISATION,
    MarginState,
    RiskAction,
    assess,
    max_price_skew_frac,
    skew_frac,
    skewed_reference,
)

_MKT = "QQQ-VOL-PERP"


def _state(**kw):
    kw.setdefault("equity_usd", 1_000.0)
    kw.setdefault("used_margin_usd", 0.0)
    return MarginState(**kw)


# --------------------------------------------------------------------------- #
# The core claim: skew with price, never with size
# --------------------------------------------------------------------------- #
def test_size_skewing_would_destroy_depth_credit_and_price_skewing_does_not():
    """The reason this module skews in price. Demonstrated, not asserted.

    Depth credit is min(bid, ask). Halving the bid to lean short halves the
    credit; sliding both quotes down leaves it untouched.
    """
    oracle = 0.07
    size = 100.0

    balanced = depth_credit_usd(
        [OrderIntent(_MKT, Side.BUY, oracle * 0.9975, size, False)],
        [OrderIntent(_MKT, Side.SELL, oracle * 1.0025, size, False)],
        oracle,
    )

    # Leaning short by SIZE.
    sized = depth_credit_usd(
        [OrderIntent(_MKT, Side.BUY, oracle * 0.9975, size / 2, False)],
        [OrderIntent(_MKT, Side.SELL, oracle * 1.0025, size, False)],
        oracle,
    )

    # Leaning short by PRICE: both legs slide down, sizes stay equal.
    shift = 1.0 - 0.01
    priced = depth_credit_usd(
        [OrderIntent(_MKT, Side.BUY, oracle * 0.9975 * shift, size, False)],
        [OrderIntent(_MKT, Side.SELL, oracle * 1.0025 * shift, size, False)],
        oracle,
    )

    assert sized < balanced * 0.6, "size skew truncates the minimum"
    assert priced == pytest.approx(balanced, rel=0.02), "price skew does not"


def test_long_inventory_leans_the_pair_down():
    assert skew_frac(50.0, 100.0) < 0.0


def test_short_inventory_leans_the_pair_up():
    assert skew_frac(-50.0, 100.0) > 0.0


def test_a_flat_book_is_not_skewed():
    assert skew_frac(0.0, 100.0) == 0.0


def test_skew_is_linear_in_position():
    quarter = skew_frac(25.0, 100.0)
    half = skew_frac(50.0, 100.0)
    assert half == pytest.approx(2 * quarter)


def test_skew_is_clamped_at_the_limit_not_extrapolated():
    """Past the limit the skew stops; it never pushes a leg out."""
    at = skew_frac(100.0, 100.0)
    way_past = skew_frac(10_000.0, 100.0)
    assert at == pytest.approx(way_past)


def test_the_ceiling_keeps_the_trailing_leg_inside_the_ring():
    ring, half_spread = 2.0, 0.25
    ceiling = max_price_skew_frac(ring, half_spread)
    assert ceiling == pytest.approx((2.0 - 0.25) / 100.0)
    # Worst case: full skew, trailing leg a half-spread behind.
    trailing_pct = (ceiling * 100.0) + half_spread
    assert trailing_pct <= ring + 1e-9


def test_a_spread_wider_than_the_ring_allows_no_skew_rather_than_a_negative():
    assert max_price_skew_frac(2.0, 5.0) == 0.0
    assert skew_frac(50.0, 100.0, ring_pct=2.0, half_spread_pct=5.0) == 0.0


def test_no_position_limit_means_no_skew_not_a_division_by_zero():
    assert skew_frac(50.0, 0.0) == 0.0


# --------------------------------------------------------------------------- #
# Margin
# --------------------------------------------------------------------------- #
def test_an_empty_account_is_fully_utilised_not_empty():
    """0.0 would read as "lots of room" to every threshold."""
    empty = MarginState(equity_usd=0.0, used_margin_usd=0.0)
    assert empty.utilisation() == 1.0


def test_normal_below_the_add_risk_line():
    d = assess(_state(used_margin_usd=100.0), _MKT,
               base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.NORMAL
    assert d.size == 10.0


def test_reduce_only_past_the_add_risk_line():
    used = 1_000.0 * MAX_MARGIN_UTILISATION
    d = assess(_state(used_margin_usd=used), _MKT,
               base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.REDUCE_ONLY


def test_flatten_past_the_flatten_line():
    used = 1_000.0 * FLATTEN_MARGIN_UTILISATION
    d = assess(_state(used_margin_usd=used), _MKT,
               base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.FLATTEN
    assert d.size == 0.0


def test_no_equity_flattens_rather_than_sizing_against_nothing():
    d = assess(MarginState(equity_usd=0.0), _MKT,
               base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.FLATTEN


def test_the_position_limit_binds_before_the_skew_ceiling_is_exhausted():
    """At the limit we stop adding, so skew never has to do it alone."""
    d = assess(_state(positions={_MKT: 100.0}), _MKT,
               base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.REDUCE_ONLY
    assert d.skew == pytest.approx(skew_frac(100.0, 100.0))


def test_a_short_at_the_limit_also_reduces():
    d = assess(_state(positions={_MKT: -100.0}), _MKT,
               base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.REDUCE_ONLY


def test_positions_in_other_markets_do_not_bind_this_one():
    d = assess(_state(positions={"NVDA-VOL-PERP": 500.0}), _MKT,
               base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.NORMAL
    assert d.skew == 0.0


def test_flatten_outranks_the_position_limit():
    """Both apply; the unsafe one has to win."""
    used = 1_000.0 * FLATTEN_MARGIN_UTILISATION
    d = assess(_state(used_margin_usd=used, positions={_MKT: 999.0}), _MKT,
               base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.FLATTEN


def test_a_carried_session_quotes_what_it_can_afford():
    """8x initial margin out of hours, so 1/8 the size on the same equity."""
    d = assess(_state(carried=True), _MKT, base_size=80.0, max_position=100.0)
    assert d.action is RiskAction.NORMAL
    assert d.size == pytest.approx(80.0 / CARRIED_IM_MULTIPLIER)


def test_a_size_that_rounds_away_is_not_quoted():
    d = assess(_state(), _MKT, base_size=0.0, max_position=100.0)
    assert d.action is RiskAction.REDUCE_ONLY
    assert d.size == 0.0


def test_a_junk_position_value_does_not_crash_the_decision():
    d = assess(_state(positions={_MKT: None}), _MKT,
               base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.NORMAL


# --------------------------------------------------------------------------- #
# Applying the skew
# --------------------------------------------------------------------------- #
def test_the_skewed_reference_moves_the_right_way():
    assert skewed_reference(0.07, -0.01) < 0.07
    assert skewed_reference(0.07, +0.01) > 0.07


@pytest.mark.parametrize("bad", [0.0, -1.0, float("nan")])
def test_a_bad_oracle_yields_no_reference_rather_than_a_plausible_one(bad):
    assert skewed_reference(bad, 0.0) is None


def test_a_skew_that_would_invert_the_price_yields_nothing():
    assert skewed_reference(0.07, -1.0) is None
    assert skewed_reference(0.07, -2.0) is None


def test_an_unskewed_reference_is_the_oracle():
    assert skewed_reference(0.07, 0.0) == pytest.approx(0.07)
    assert math.isfinite(skewed_reference(0.07, 0.0))


# --------------------------------------------------------------------------- #
# [review] Every unreadable number must mean "no room", never "lots of room"
# --------------------------------------------------------------------------- #
@pytest.mark.parametrize("bad", [float("nan"), float("inf"), float("-inf")])
def test_non_finite_margin_reads_as_fully_utilised(bad):
    """NaN makes BOTH threshold comparisons false, so assess() returned
    NORMAL and kept adding risk against an unreadable account."""
    st = MarginState(equity_usd=1_000.0, used_margin_usd=bad)
    assert st.utilisation() == 1.0
    d = assess(st, _MKT, base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.FLATTEN


@pytest.mark.parametrize("bad", [float("nan"), float("inf")])
def test_non_finite_equity_reads_as_fully_utilised(bad):
    assert MarginState(equity_usd=bad, used_margin_usd=0.0).utilisation() == 1.0
