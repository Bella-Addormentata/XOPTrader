"""[PREFLIGHT] Legs judged against the oracle the venue will actually use.

The measured failure this exists for (2026-08-31, live):

    Price 0.047 is outside the allowed oracle band
    [0.0420191330879910865, 0.0464421997288322535]
    (+/-5% of oracle 0.04423066640841167)

Our fetch was seconds old at the time. Age is not divergence: while
volatility collapsed ~20%/min a "fresh" read was already 5% behind, and
band_guard's clamp -- anchored to that same stale read -- could not help.
"""

from __future__ import annotations

from gui.services.permuto.preflight import (
    RESERVE_FRAC,
    latest_oracle,
    preflight_leg_price,
    stand_down,
)

BAND = 5.0


def _pf(price, oracle, latency=0.25, vel=0.0, is_buy=False):
    return preflight_leg_price(price, oracle, band_pct=BAND,
                               latency_s=latency,
                               velocity_pct_per_s=vel, is_buy=is_buy)


# --------------------------------------------------------------------------- #
# The live case
# --------------------------------------------------------------------------- #

def test_the_measured_rejection_is_re_anchored_instead_of_sent():
    # Exactly the refused order: 0.047 against a venue oracle of 0.04423.
    out = _pf(0.047, 0.04423066640841167, latency=0.25, vel=0.5)
    assert not out.dropped
    assert out.changed
    # Inside the venue's real band, with the reserve kept back.
    assert 0.0420191330879910865 <= out.price <= 0.0464421997288322535
    assert "re-anchored" in out.reason


def test_a_leg_already_inside_the_band_is_untouched():
    out = _pf(0.0450, 0.04423066640841167, latency=0.25, vel=0.1)
    assert not out.changed and not out.dropped
    assert out.price == 0.0450


# --------------------------------------------------------------------------- #
# Stand-down
# --------------------------------------------------------------------------- #

def test_a_market_moving_faster_than_the_band_stands_down():
    # 20%/min ~= 0.33%/s. With a slow 12s round trip, projected drift is
    # ~4% against a 5% band: no offset is safe.
    assert stand_down(BAND, 12.0, 0.33) is True
    out = _pf(0.045, 0.0442, latency=12.0, vel=0.33)
    assert out.dropped
    assert "exceeds" in out.reason


def test_a_calm_market_never_stands_down():
    assert stand_down(BAND, 0.25, 0.0) is False
    assert stand_down(BAND, 0.25, 0.05) is False


def test_a_zero_band_stands_down_rather_than_dividing_by_it():
    assert stand_down(0.0, 0.1, 0.1) is True


def test_negative_inputs_cannot_manufacture_headroom():
    assert stand_down(BAND, -100.0, 5.0) is False    # clamped to 0 drift
    out = _pf(0.045, 0.0442, latency=-1.0, vel=-1.0)
    assert not out.dropped


# --------------------------------------------------------------------------- #
# The reserve
# --------------------------------------------------------------------------- #

def test_the_usable_width_keeps_a_reserve_under_the_band():
    # Calm, instant: usable is band x (1 - reserve), never the full band --
    # the venue resamples on its own clock and our round trip is an
    # estimate, not a guarantee.
    out = _pf(999.0, 1.0, latency=0.0, vel=0.0)
    assert out.changed
    expected = 1.0 + BAND * (1.0 - RESERVE_FRAC) / 100.0
    assert abs(out.price - expected) < 1e-12


def test_drift_eats_into_the_usable_width():
    calm = _pf(999.0, 1.0, latency=0.0, vel=0.0).price
    fast = _pf(999.0, 1.0, latency=2.0, vel=0.5).price
    assert fast < calm, "projected drift must narrow the window"


def test_a_bid_is_clamped_up_toward_the_oracle():
    out = _pf(0.001, 1.0, latency=0.0, vel=0.0, is_buy=True)
    assert out.changed and out.price < 1.0
    assert out.price == 1.0 - BAND * (1.0 - RESERVE_FRAC) / 100.0


# --------------------------------------------------------------------------- #
# Degenerate inputs
# --------------------------------------------------------------------------- #

def test_junk_price_or_oracle_is_dropped_not_guessed():
    assert _pf(0.0, 1.0).dropped
    assert _pf(1.0, 0.0).dropped
    assert _pf(-1.0, 1.0).dropped


# --------------------------------------------------------------------------- #
# Oracle source selection
# --------------------------------------------------------------------------- #

def test_the_fresh_read_wins_when_present():
    assert latest_oracle({"M": 2.0}, {"M": 1.0}, "M") == 2.0


def test_a_failed_fresh_fetch_falls_back_to_the_ticks_read():
    # A hiccup on the extra request must not cost a quoting cycle.
    assert latest_oracle(None, {"M": 1.0}, "M") == 1.0
    assert latest_oracle({}, {"M": 1.0}, "M") == 1.0
    assert latest_oracle({"OTHER": 3.0}, {"M": 1.0}, "M") == 1.0


def test_junk_in_either_source_is_skipped():
    assert latest_oracle({"M": 0.0}, {"M": 1.0}, "M") == 1.0
    assert latest_oracle({"M": "x"}, {"M": 1.0}, "M") == 1.0
    assert latest_oracle({"M": 0.0}, {"M": 0.0}, "M") == 0.0


# --------------------------------------------------------------------------- #
# The aggressive side is limited by the RING, not the oracle midpoint.
# --------------------------------------------------------------------------- #

def test_a_bid_is_never_re_anchored_past_the_ring():
    # The bug: clamping a too-high bid to the BAND ceiling (3.25% over)
    # produced "aggressive OUTSIDE the +/-2% ring" at place time.
    out = _pf(0.10, 0.09, latency=0.0, vel=0.0, is_buy=True)
    assert out.changed
    assert out.price <= 0.09 * 1.02 + 1e-12


def test_an_ask_may_still_sit_just_under_the_oracle():
    # Inventory skew leans the whole ladder down when long; an ask a shade
    # below the oracle is a legal maker quote and must survive untouched.
    # Capping the ask at the midpoint would have silently killed the skew.
    out = _pf(0.0995, 0.10, latency=0.0, vel=0.0, is_buy=False)
    assert not out.changed and not out.dropped
    assert out.price == 0.0995


def test_an_ask_far_below_the_oracle_is_pulled_back_to_the_ring():
    out = _pf(0.05, 0.10, latency=0.0, vel=0.0, is_buy=False)
    assert out.changed
    assert out.price >= 0.10 * 0.98 - 1e-12
