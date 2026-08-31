"""The anti-cross controller must widen, relax, and never leave the ring."""

from gui.services.permuto.cross_backoff import (
    BACKOFF_DECAY, BACKOFF_FLOOR_PCT, BACKOFF_STEP_PCT, CrossBackoff,
    headroom_pct,
)

_MKT = "QQQ-VOL-PERP"


def test_it_starts_at_the_configured_placement():
    assert CrossBackoff().offset_pct(_MKT) == 0.0


def test_a_crossing_refusal_widens_by_one_step():
    b = CrossBackoff()
    assert b.observe_cross(_MKT, headroom_pct=1.75) == BACKOFF_STEP_PCT
    assert b.offset_pct(_MKT) == BACKOFF_STEP_PCT


def test_repeated_refusals_keep_widening_until_the_ring_stops_them():
    b = CrossBackoff()
    for _ in range(50):
        b.observe_cross(_MKT, headroom_pct=1.75)
    assert b.offset_pct(_MKT) == 1.75, \
        "backoff escaped the ring; a leg outside it earns zero depth"


def test_it_never_exceeds_the_headroom_even_on_the_first_step():
    b = CrossBackoff()
    # Headroom smaller than a single step: the step must be truncated, not
    # taken in full and then apologised for.
    assert b.observe_cross(_MKT, headroom_pct=0.10) == 0.10


def test_zero_headroom_means_no_retreat_is_possible():
    b = CrossBackoff()
    b.observe_cross(_MKT, headroom_pct=1.0)
    assert b.offset_pct(_MKT) > 0.0
    # The spread later widens past the ring -- there is nowhere left to go,
    # and pretending otherwise would push the leg out of the credit ring.
    assert b.observe_cross(_MKT, headroom_pct=0.0) == 0.0
    assert b.offset_pct(_MKT) == 0.0


def test_a_clean_tick_relaxes_the_backoff():
    b = CrossBackoff()
    b.observe_cross(_MKT, headroom_pct=1.75)
    before = b.offset_pct(_MKT)
    after = b.observe_clean(_MKT)
    assert after < before
    assert after == before * BACKOFF_DECAY


def test_it_returns_all_the_way_to_zero_rather_than_leaving_a_residue():
    b = CrossBackoff()
    b.observe_cross(_MKT, headroom_pct=1.75)
    for _ in range(200):
        b.observe_clean(_MKT)
    assert b.offset_pct(_MKT) == 0.0, \
        "multiplicative decay left a permanent residue"


def test_relaxing_an_untouched_market_is_a_no_op():
    b = CrossBackoff()
    assert b.observe_clean("NVDA-VOL-PERP") == 0.0


def test_markets_do_not_share_a_backoff():
    b = CrossBackoff()
    b.observe_cross(_MKT, headroom_pct=1.75)
    assert b.offset_pct("NVDA-VOL-PERP") == 0.0, \
        "one market's crossing widened a sibling that never crossed"


def test_forget_drops_the_learned_offset():
    b = CrossBackoff()
    b.observe_cross(_MKT, headroom_pct=1.75)
    b.forget(_MKT)
    assert b.offset_pct(_MKT) == 0.0


# --------------------------------------------------------------------------- #
# headroom
# --------------------------------------------------------------------------- #

def test_headroom_is_the_ring_less_the_spread():
    assert headroom_pct(2.0, 0.25) == 1.75


def test_headroom_subtracts_the_skew_already_applied():
    # skew_frac is a FRACTION (0.0096 == 0.96%), the units risk.skew_frac uses.
    assert abs(headroom_pct(2.0, 0.25, 0.0096) - 0.79) < 1e-9


def test_headroom_never_goes_negative():
    assert headroom_pct(2.0, 3.0) == 0.0, \
        "a negative headroom would invert the retreat into an advance"
    assert headroom_pct(0.0, 0.25) == 0.0


def test_a_backoff_plus_spread_plus_skew_stays_inside_the_ring():
    """The property that actually matters, stated as arithmetic.

    Whatever the controller returns, placement + skew + backoff must land
    inside the credit ring -- otherwise the leg rests and scores nothing,
    which is the failure this module exists to avoid, arriving through the
    module itself.
    """
    ring, spread = 2.0, 0.25
    for skew in (0.0, 0.005, 0.0096, 0.0175):
        room = headroom_pct(ring, spread, skew)
        b = CrossBackoff()
        for _ in range(50):
            b.observe_cross(_MKT, headroom_pct=room)
        total = spread + abs(skew) * 100.0 + b.offset_pct(_MKT)
        assert total <= ring + 1e-9, (
            "spread %.2f + skew %.2f%% + backoff %.2f = %.4f, outside the "
            "%.2f%% ring" % (spread, skew * 100.0, b.offset_pct(_MKT),
                             total, ring))
