"""Order-legality tests.

Every failure mode here is silent from our side: an order rejected at place, or
accepted and then purged on an oracle move, looks the same locally as one
resting and earning. So these assert the venue's rules rather than our
intentions.
"""

from __future__ import annotations

import pytest

from gui.services.permuto.orders import (
    OrderIntent,
    Placement,
    Side,
    classify_placement,
    depth_credit_usd,
    notional_usd,
    quote_ladder,
)

ORACLE = 0.10


# --------------------------------------------------------------------------- #
# The three bands
# --------------------------------------------------------------------------- #

def test_a_bid_below_the_oracle_is_passive_and_always_legal():
    assert classify_placement(Side.BUY, 0.099, ORACLE) is Placement.PASSIVE
    assert classify_placement(Side.SELL, 0.101, ORACLE) is Placement.PASSIVE


def test_aggressive_inside_the_ring_is_allowed():
    # A bid ABOVE the oracle is aggressive; inside 2% that is fine.
    assert classify_placement(Side.BUY, 0.101, ORACLE) is Placement.AGGRESSIVE
    assert classify_placement(Side.SELL, 0.099, ORACLE) is Placement.AGGRESSIVE


def test_aggressive_outside_the_ring_is_purge_risk_not_merely_rejected():
    """THE trap. Outside the ring an aggressive rest is refused at place --
    and if one ever rests, an oracle move purges it. The book empties without
    us doing anything, and nothing local says so."""
    assert classify_placement(Side.BUY, 0.103, ORACLE) is Placement.PURGE_RISK
    assert classify_placement(Side.SELL, 0.097, ORACLE) is Placement.PURGE_RISK


def test_outside_the_legal_band_is_illegal_on_either_side():
    assert classify_placement(Side.BUY, 0.94 * ORACLE, ORACLE) is Placement.ILLEGAL
    assert classify_placement(Side.SELL, 1.06 * ORACLE, ORACLE) is Placement.ILLEGAL


def test_the_boundaries_are_inclusive():
    """The venue expresses these as "within N%"; rejecting an order sitting
    exactly on a limit is the surprising direction."""
    assert classify_placement(Side.BUY, ORACLE * 1.02, ORACLE) is Placement.AGGRESSIVE
    assert classify_placement(Side.BUY, ORACLE * 0.95, ORACLE) is Placement.PASSIVE
    assert classify_placement(Side.BUY, ORACLE * 1.0201, ORACLE) is Placement.PURGE_RISK


def test_no_fresh_oracle_means_no_opinion():
    """A stale or missing oracle must not be treated as permission. Orders
    placed against one look fine locally and score nothing."""
    for bad in (0.0, -1.0, float("nan")):
        assert classify_placement(Side.BUY, 0.1, bad) is Placement.ILLEGAL


# --------------------------------------------------------------------------- #
# Depth credit is min(bid, ask) -- the property the whole strategy rests on
# --------------------------------------------------------------------------- #

def test_depth_is_the_minimum_of_the_two_sides_not_the_sum():
    bids = [OrderIntent("QQQ-VOL-PERP", Side.BUY, 0.0995, 10_000)]   # ~$995
    asks = [OrderIntent("QQQ-VOL-PERP", Side.SELL, 0.1005, 5_000)]   # ~$502
    credit = depth_credit_usd(bids, asks, ORACLE)
    assert credit == pytest.approx(502.5, rel=1e-3)


def test_a_lifted_side_earns_nothing_for_the_whole_market():
    """Not 'less' -- nothing. A filled bid stops accrual even though the ask
    is untouched, which is why restoring a side is urgent rather than tidy."""
    asks = [OrderIntent("QQQ-VOL-PERP", Side.SELL, 0.1005, 50_000)]
    assert depth_credit_usd([], asks, ORACLE) == 0.0


def test_legs_outside_the_ring_earn_nothing():
    """They may be perfectly legal placements and still score zero -- the
    band and the credit ring are different numbers."""
    bids = [OrderIntent("QQQ-VOL-PERP", Side.BUY, ORACLE * 0.97, 10_000)]
    asks = [OrderIntent("QQQ-VOL-PERP", Side.SELL, ORACLE * 1.03, 10_000)]
    assert classify_placement(Side.BUY, ORACLE * 0.97, ORACLE) is Placement.PASSIVE
    assert depth_credit_usd(bids, asks, ORACLE) == 0.0


def test_reduce_only_legs_do_not_count_as_liquidity():
    """They shed inventory; counting them would flatter a retreating book."""
    bids = [OrderIntent("QQQ-VOL-PERP", Side.BUY, 0.0995, 10_000, reduce_only=True)]
    asks = [OrderIntent("QQQ-VOL-PERP", Side.SELL, 0.1005, 10_000)]
    assert depth_credit_usd(bids, asks, ORACLE) == 0.0


# --------------------------------------------------------------------------- #
# The ladder
# --------------------------------------------------------------------------- #

def test_the_ladder_earns_what_it_was_asked_for():
    legs = quote_ladder("QQQ-VOL-PERP", ORACLE, target_depth_usd=1000.0)
    bids = [o for o in legs if o.side is Side.BUY]
    asks = [o for o in legs if o.side is Side.SELL]
    # Within one lot per leg of the target, and never ABOVE it: sizes floor
    # to the venue's lot grid, so quantisation may shave a few cents but must
    # not promise notional that was never priced.
    credit = depth_credit_usd(bids, asks, ORACLE)
    assert credit <= 1000.0 + 1e-9
    assert credit == pytest.approx(1000.0, abs=1.0)


def test_the_ladder_is_symmetric_because_the_minimum_is_what_scores():
    """Any asymmetry is capital at risk on the heavier side earning nothing."""
    legs = quote_ladder("QQQ-VOL-PERP", ORACLE, 1000.0)
    bid_notional = sum(o.notional for o in legs if o.side is Side.BUY)
    ask_notional = sum(o.notional for o in legs if o.side is Side.SELL)
    # Symmetric to within one lot per level -- tick/lot quantisation rounds
    # each side on its own grid, so exact equality is no longer the invariant;
    # bounded asymmetry is.
    assert bid_notional == pytest.approx(ask_notional, abs=1.0)


def test_every_ladder_leg_is_strictly_inside_the_ring():
    """A leg outside earns nothing AND can be purged on an oracle move, so
    asking for more levels than the ring holds must tighten the ladder rather
    than emit invisible legs."""
    legs = quote_ladder("QQQ-VOL-PERP", ORACLE, 1000.0, levels=12,
                        level_step_pct=1.0)
    for leg in legs:
        assert classify_placement(leg.side, leg.price, ORACLE) in (
            Placement.PASSIVE, Placement.AGGRESSIVE)
        assert abs(leg.price - ORACLE) / ORACLE * 100.0 <= 2.0


def test_a_degenerate_request_yields_no_orders_rather_than_bad_ones():
    assert quote_ladder("QQQ-VOL-PERP", 0.0, 1000.0) == []
    assert quote_ladder("QQQ-VOL-PERP", ORACLE, 0.0) == []
    assert quote_ladder("QQQ-VOL-PERP", ORACLE, 1000.0, levels=0) == []


def test_notional_rejects_non_finite_so_a_bad_quote_cannot_pass_a_cap():
    assert notional_usd(float("nan"), 100) == 0.0
    assert notional_usd(float("inf"), 100) == 0.0
    assert notional_usd(0.1, -5) == 0.0


def test_the_market_field_is_the_symbol_not_the_oracle_ticker():
    """/info/meta carries both and they differ by suffix; order routes want
    QQQ-VOL-PERP. Getting this wrong is an HTTP 400 on every single order."""
    legs = quote_ladder("QQQ-VOL-PERP", ORACLE, 100.0, levels=1)
    assert legs and all(o.market.endswith("-PERP") for o in legs)


# --------------------------------------------------------------------------- #
# Credit is measured per side, so the LEG decides -- not the bucket
# --------------------------------------------------------------------------- #

def test_a_sell_intent_in_the_bid_book_is_refused_rather_than_scored():
    """119-7. Classification came from the iterable a leg was handed in
    through, never from ``leg.side``. A SELL priced above the oracle, passed
    as a bid, was therefore scored under the BUY rule -- aggressive, inside
    the ring -- and counted as bid liquidity that does not exist."""
    stray = OrderIntent("QQQ-VOL-PERP", Side.SELL, ORACLE * 1.005, 10_000)
    asks = [OrderIntent("QQQ-VOL-PERP", Side.SELL, ORACLE * 1.005, 10_000)]
    with pytest.raises(ValueError, match="sell"):
        depth_credit_usd([stray], asks, ORACLE)


def test_two_sell_sides_can_never_produce_balanced_depth():
    """THE consequence. min(bid, ask) is the entire scoring model, so a
    one-sided book that reads as two-sided overstates the one number this
    module exists to keep honest."""
    sells = [OrderIntent("QQQ-VOL-PERP", Side.SELL, ORACLE * 1.005, 10_000)]
    with pytest.raises(ValueError):
        depth_credit_usd(sells, list(sells), ORACLE)


def test_a_wrong_sided_reduce_only_leg_is_still_a_caller_bug():
    """Skipping it quietly would hide the mis-bucketing that produced it."""
    stray = OrderIntent("QQQ-VOL-PERP", Side.BUY, ORACLE * 0.995, 10.0,
                        reduce_only=True)
    with pytest.raises(ValueError):
        depth_credit_usd([], [stray], ORACLE)


# --------------------------------------------------------------------------- #
# Ladder inputs -- every tuning knob, not the three that happened to be checked
# --------------------------------------------------------------------------- #

def _no_leg_is_malformed(legs):
    for leg in legs:
        assert leg.price == leg.price and leg.price > 0.0
        assert leg.price not in (float("inf"), float("-inf"))
        assert leg.size == leg.size and leg.size > 0.0
        assert leg.size not in (float("inf"), float("-inf"))


def test_a_negative_offset_cannot_invert_the_ladder():
    """119-8. A negative first offset put the BUY above the oracle and the
    SELL below it. Nothing downstream catches that: both legs stay inside the
    band and the ring, so batch.py classifies them as legal and the crossed
    pair is sent -- and ``first_offset_pct`` reaches here from configuration
    (runner.py's ``_half_spread_pct``), not from a literal."""
    legs = quote_ladder("QQQ-VOL-PERP", ORACLE, 1000.0, first_offset_pct=-0.25)
    _no_leg_is_malformed(legs)
    bids = [o.price for o in legs if o.side is Side.BUY]
    asks = [o.price for o in legs if o.side is Side.SELL]
    assert not any(b > a for b in bids for a in asks)


@pytest.mark.parametrize("kwargs", [
    {"first_offset_pct": float("nan")},
    {"level_step_pct": float("nan")},
    {"first_offset_pct": float("inf")},
    {"level_step_pct": float("-inf")},
    {"first_offset_pct": -0.25},
    {"level_step_pct": -0.5},
    {"ring_pct": float("nan")},
    {"ring_pct": 0.0},
    {"ring_pct": -2.0},
    {"ring_pct": 250.0},
])
def test_out_of_domain_tuning_quotes_nothing(kwargs):
    """``min(nan, x)`` is nan in Python -- the ``x < nan`` comparison is False
    -- so a NaN offset survived the ring clamp and landed in both prices. A
    ring above 100% lifts that clamp past the oracle and makes the bid price
    negative."""
    legs = quote_ladder("QQQ-VOL-PERP", ORACLE, 1000.0, **kwargs)
    assert legs == []


@pytest.mark.parametrize("oracle,target", [
    (float("inf"), 1000.0),
    (float("nan"), 1000.0),
    (ORACLE, float("inf")),
    (ORACLE, float("nan")),
])
def test_non_finite_oracle_or_target_quotes_nothing(oracle, target):
    """``inf > 0.0`` passes a bare positivity check, and every leg built from
    one is infinite in price and zero in size, or infinite in size."""
    assert quote_ladder("QQQ-VOL-PERP", oracle, target) == []


def test_prices_land_on_the_tick_grid_and_sizes_on_the_lot_grid():
    """[release review] The live venue declares tick_size 0.0001 and
    lot_size 1, and nothing honoured either -- 16-decimal prices and
    fractional sizes. A strict validator rejects the whole batch: zero
    orders for 102 hours."""
    legs = quote_ladder("QQQ-VOL-PERP", 0.1544391445921985, 1000.0,
                        tick_size=0.0001, lot_size=1.0)
    assert legs
    for leg in legs:
        ticks = leg.price / 0.0001
        assert abs(ticks - round(ticks)) < 1e-6, leg.price
        assert leg.size == int(leg.size), leg.size
        assert leg.size >= 1


def test_quantisation_rounds_away_from_the_oracle():
    """Never sharper than the price risk approved: bid floors DOWN, ask
    ceils UP."""
    oracle = 0.1544391445921985
    legs = quote_ladder("QQQ-VOL-PERP", oracle, 1000.0, levels=1,
                        tick_size=0.0001, lot_size=1.0)
    bid = next(o for o in legs if o.side is Side.BUY)
    ask = next(o for o in legs if o.side is Side.SELL)
    assert bid.price <= oracle * (1.0 - 0.25 / 100.0) + 1e-12
    assert ask.price >= oracle * (1.0 + 0.25 / 100.0) - 1e-12


def test_an_observed_full_ring_cap_reserves_the_quantisation_tick():
    oracle = 0.1544391445921985
    legs = quote_ladder(
        "QQQ-VOL-PERP", oracle, 1000.0,
        levels=1,
        first_offset_pct=2.0,
        max_offset_pct=2.0,
        tick_size=0.0001,
        lot_size=1.0,
    )
    assert legs
    for leg in legs:
        deviation = abs(leg.price / oracle - 1.0) * 100.0
        assert deviation <= 2.0 + 1e-9, leg


def test_a_degenerate_quantised_pair_is_skipped_not_sent():
    """A tick wider than the spread crosses or zeroes the pair; the level is
    dropped rather than shipped."""
    assert quote_ladder("X", 0.0002, 1000.0, levels=1,
                        tick_size=0.01, lot_size=1.0) == []


def test_an_unusable_tick_or_lot_falls_back_to_the_documented_default():
    legs = quote_ladder("QQQ-VOL-PERP", 0.15, 1000.0,
                        tick_size=float("nan"), lot_size=0.0)
    assert legs
    for leg in legs:
        ticks = leg.price / 0.0001
        assert abs(ticks - round(ticks)) < 1e-6

