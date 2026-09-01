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
    # Worst case: full skew, trailing leg a half-spread behind.
    trailing_pct = (ceiling * 100.0) + half_spread
    assert trailing_pct <= ring + 1e-9


def test_the_ceiling_stays_below_the_requote_trigger():
    """[release review] The ring-edge ceiling (1.75% at defaults) sat ABOVE
    the drift trigger (ring * REQUOTE_AT_RING_FRACTION = 1.2%), so a fully
    skewed quote was born already past the trigger -- re-quoted every tick,
    an endless cancel/replace churn that never rested long enough to earn
    depth. The skew must leave the trigger headroom, so a skewed pair rests
    until the ORACLE moves."""
    from gui.services.permuto.quoting import REQUOTE_AT_RING_FRACTION
    ring, half_spread = 2.0, 0.25
    ceiling = max_price_skew_frac(ring, half_spread)
    trigger = ring * REQUOTE_AT_RING_FRACTION / 100.0
    assert ceiling < trigger, "a full skew is instantly past its own trigger"

    # [review 2026-09-01] PLACEMENT AND ROUNDING COUNT TOO. decide()
    # re-quotes on abs(leg_price - oracle), and the leg sits a half-spread
    # beyond the skewed midpoint before being CEILED onto the grid. At the
    # defaults with oracle 0.07 and a 95% short that was 1.2857% against a
    # 1.20% trigger -- born past it, replaced every tick.
    import math as _math
    for half_spread in (0.05, 0.25, 0.40, 0.75):
        # [review] The tick values matter. With the tick term DROPPED the
        # ceiling becomes (0.96 - spread), so the total collapses to
        # 0.96 + tick% -- which only breaches 1.20% once the tick exceeds
        # 0.24% of the oracle. At 0.0001/0.07 that is 0.143% and the
        # broken version still passes; my first version of this loop
        # stopped there and the mutation survived it.
        #
        # QQQ-VOL traded as low as 0.01615 in the session sampled on
        # 2026-08-31, where one tick is 0.62% -- so the small-oracle case
        # is the real one, not a contrived edge.
        for tick_frac in (0.0, 0.0001 / 0.15, 0.0001 / 0.07,
                          0.0001 / 0.0416, 0.0001 / 0.01615):
            ceil_ = max_price_skew_frac(ring, half_spread, tick_frac)
            total = (half_spread + ceil_ * 100.0 + tick_frac * 100.0)
            trigger_pct = ring * REQUOTE_AT_RING_FRACTION
            # The honest property: the CEILING must never contribute to a
            # breach. Where spread + tick alone already exceed the trigger
            # -- 0.75% placement against a 0.62% tick at oracle 0.01615 --
            # no skew allowance can rescue it, and the function's only
            # correct answer is zero. Asserting the total unconditionally
            # would demand the impossible and fail on the fixed code, the
            # same over-strong shape as the ring-bound property earlier.
            assert total <= trigger_pct + 1e-9 or ceil_ == 0.0, (
                "spread %.2f + skew %.4f + tick %.4f = %.4f%%, past the "
                "%.2f%% trigger with a NON-zero ceiling"
                % (half_spread, ceil_ * 100.0, tick_frac * 100.0,
                   total, trigger_pct))

    # A spread so wide the placement ALONE clears the trigger leaves no
    # room for any skew at all -- zero, not a sliver off the ring edge.
    assert max_price_skew_frac(2.0, 1.9) == 0.0


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


def test_a_negative_used_margin_reads_as_fully_utilised():
    """Finite, so it passes an isfinite check -- and then the division gives
    NEGATIVE utilisation, which is below every threshold and reads as
    unlimited headroom."""
    st = MarginState(equity_usd=1_000.0, used_margin_usd=-500.0)
    assert st.utilisation() == 1.0
    d = assess(st, _MKT, base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.FLATTEN


@pytest.mark.parametrize("bad", [float("nan"), float("inf"), float("-inf")])
def test_an_unreadable_position_flattens_rather_than_skewing(bad):
    """NaN fails the limit comparison and is then clamped into a finite
    extreme skew, so assess() returned NORMAL on inventory it could not
    read."""
    d = assess(_state(positions={_MKT: bad}), _MKT,
               base_size=10.0, max_position=100.0)
    assert d.action is RiskAction.FLATTEN


# --------------------------------------------------------------------------- #
# The portfolio budget: no single market has to breach its own limit for the
# book to breach the account.
# --------------------------------------------------------------------------- #
def test_the_portfolio_budget_binds_before_any_market_does():
    """[audit] max_position_usd is PER MARKET and nothing aggregated it.

    Three markets at the shipped 250,000 authorised 750,000 of exposure on
    a 500,000 account -- 1.5x the equity before the venue's 8x carried
    multiplier touches it, and with every individual market perfectly
    inside its own limit. That is the shape of a liquidation nothing in
    the per-market checks can see.
    """
    from gui.services.permuto.risk import (PORTFOLIO_MAX_EXPOSURE_FRACTION,
                                           portfolio_cap_usd)
    equity, per_market = 500_000.0, 250_000.0
    budget = equity * PORTFOLIO_MAX_EXPOSURE_FRACTION

    # Flat: a market may use its own limit, capped by the budget.
    assert portfolio_cap_usd(equity, "A", per_market, {}) == min(per_market,
                                                                 budget)
    # Neighbours holding exposure take it away, one for one.
    assert portfolio_cap_usd(equity, "C", per_market,
                             {"A": 100_000.0, "B": 50_000.0}) == budget - 150_000.0
    # Once the budget is spent there is no room at all, however far this
    # market is from its own limit.
    assert portfolio_cap_usd(equity, "C", per_market,
                             {"A": budget}) == 0.0
    # Never negative, and never LARGER than what was asked for.
    assert portfolio_cap_usd(equity, "C", per_market,
                             {"A": 10 * budget}) == 0.0
    assert portfolio_cap_usd(equity, "A", 1_000.0, {}) == 1_000.0


def test_the_portfolio_budget_fails_closed_on_anything_unreadable():
    """Authorising exposure against a number nobody can see is how the
    previous account died."""
    from gui.services.permuto.risk import portfolio_cap_usd
    for equity in (0.0, -1.0, float("nan"), float("inf")):
        assert portfolio_cap_usd(equity, "A", 250_000.0, {}) == 0.0
    for junk in ("lots", None, float("nan"), float("inf")):
        assert portfolio_cap_usd(500_000.0, "A", 250_000.0,
                                 {"B": junk}) == 0.0
    for cap in (0.0, -1.0, float("nan")):
        assert portfolio_cap_usd(500_000.0, "A", cap, {}) == 0.0


def test_the_market_being_sized_is_not_counted_against_itself():
    """Its own position is what the per-market cap already governs; adding
    it here would double-charge and shrink the book for no reason."""
    from gui.services.permuto.risk import portfolio_cap_usd
    with_self = portfolio_cap_usd(500_000.0, "A", 250_000.0,
                                  {"A": 200_000.0})
    without = portfolio_cap_usd(500_000.0, "A", 250_000.0, {})
    assert with_self == without
