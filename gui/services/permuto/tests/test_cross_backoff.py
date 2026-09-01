"""The anti-cross controller must widen, relax, and never leave the ring."""

from gui.services.permuto.risk import max_price_skew_frac
from gui.services.permuto.cross_backoff import (
    BACKOFF_DECAY, BACKOFF_STEP_PCT, CrossBackoff,
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
    # Tolerance, not equality: the bound is computed as a ratio now, and
    # (1.02 - 1.0) * 100 is 2.0000000000000018 in binary floating point.
    assert abs(headroom_pct(2.0, 0.25) - 1.75) < 1e-9


def test_headroom_composes_the_skew_multiplicatively():
    """[review] The offsets compose as products, not sums.

    The ladder prices against an already-skewed reference, so the trailing
    leg is at oracle * (1 + skew) * (1 + offset). Subtracting additively
    (2.0 - 0.25 - 0.96 = 0.79) let the ask land at
    (1.0096 * 1.0104 - 1) = 2.0100% -- outside the 2% credit ring, earning
    nothing, which is exactly what this bound exists to prevent.
    """
    room = headroom_pct(2.0, 0.25, 0.0096)
    assert room < 0.79, "additive arithmetic overstates the room"
    # And the composed result must genuinely land inside the ring.
    landed = (1.0096 * (1.0 + (0.25 + room) / 100.0) - 1.0) * 100.0
    assert landed <= 2.0 + 1e-9, "leg lands at %.4f%%, outside the ring" % landed


def test_the_composed_bound_holds_across_every_skew_that_can_occur():
    """The property, over the domain the system can actually produce.

    The ceiling is risk.max_price_skew_frac, which is itself capped below
    the ring edge AND below the re-quote trigger -- 0.96% at defaults. Past
    about 1.75% the configured SPREAD alone already leaves the ring with
    zero backoff, which is not this function's doing and not something it
    can fix; testing there would assert an impossibility and fail for the
    wrong reason.
    """
    ring, spread = 2.0, 0.25
    ceiling = max_price_skew_frac(ring, spread)
    steps = 200
    for i in range(steps + 1):
        skew = ceiling * i / steps
        room = headroom_pct(ring, spread, skew)
        landed = ((1.0 + skew) * (1.0 + (spread + room) / 100.0) - 1.0) * 100.0
        assert landed <= ring + 1e-9, (
            "skew %.4f + spread %.2f + room %.4f lands at %.4f%%, outside "
            "the %.1f%% ring" % (skew, spread, room, landed, ring))


def test_where_there_is_no_room_the_backoff_contributes_nothing():
    """Past the point where spread+skew fills the ring, room is zero.

    The leg may still sit outside the ring on spread and skew alone -- that
    is a quoting-config problem, not a backoff one -- but the controller
    must not ADD to it.
    """
    assert headroom_pct(2.0, 0.25, 0.0175) == 0.0
    assert headroom_pct(2.0, 3.0) == 0.0


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
    for skew in (0.0, 0.005, max_price_skew_frac(ring, spread)):
        room = headroom_pct(ring, spread, skew)
        b = CrossBackoff()
        for _ in range(50):
            b.observe_cross(_MKT, headroom_pct=room)
        landed = ((1.0 + abs(skew))
                  * (1.0 + (spread + b.offset_pct(_MKT)) / 100.0)
                  - 1.0) * 100.0
        assert landed <= ring + 1e-9, (
            "spread %.2f + skew %.2f%% + backoff %.2f composes to %.4f%%, "
            "outside the %.2f%% ring" % (spread, skew * 100.0,
                                         b.offset_pct(_MKT), landed, ring))


def test_headroom_reserves_a_tick_for_ask_ceil_rounding():
    """[review] quote_ladder rounds asks UP; the bound must absorb that tick.

    For oracle=0.1544391445921985, tick=0.0001, skew=0.0096 the raw ask
    before rounding lands exactly on the ring boundary when headroom_pct
    is called without tick reservation.  math.ceil then pushes it one tick
    further out -- 2.0467% from the oracle -- outside the 2% credit ring.

    With tick reservation the bound is tightened by tick/oracle so the
    ceil'd price always lands inside the ring.
    """
    import math
    oracle = 0.1544391445921985
    tick = 0.0001
    ring = 2.0
    spread = 0.25
    skew = 0.0096

    tick_frac = tick / oracle
    room = headroom_pct(ring, spread, skew, tick_frac)

    # Simulate quote_ladder: place at oracle*(1+skew)*(1+offset/100)
    raw_ask = oracle * (1.0 + skew) * (1.0 + (spread + room) / 100.0)
    quantised_ask = math.ceil(raw_ask / tick) * tick

    pct_from_oracle = (quantised_ask / oracle - 1.0) * 100.0
    assert pct_from_oracle <= ring + 1e-9, (
        "quantised ask is %.4f%% from oracle, outside the %.1f%% ring "
        "(oracle=%.16f tick=%.4f skew=%.2f%% room=%.4f%%)"
        % (pct_from_oracle, ring, oracle, tick, skew * 100.0, room))


def test_headroom_tick_reservation_holds_across_representative_domain():
    """The quantised price stays inside the ring for every skew that occurs."""
    import math
    oracle = 0.1544391445921985
    tick = 0.0001
    ring = 2.0
    spread = 0.25
    tick_frac = tick / oracle
    ceiling = max_price_skew_frac(ring, spread)
    steps = 200
    for i in range(steps + 1):
        skew = ceiling * i / steps
        room = headroom_pct(ring, spread, skew, tick_frac)
        if room <= 0.0:
            continue
        raw_ask = oracle * (1.0 + skew) * (1.0 + (spread + room) / 100.0)
        quantised_ask = math.ceil(raw_ask / tick) * tick
        pct_from_oracle = (quantised_ask / oracle - 1.0) * 100.0
        assert pct_from_oracle <= ring + 1e-9, (
            "skew %.4f%%: quantised ask %.4f%% from oracle, outside ring"
            % (skew * 100.0, pct_from_oracle))

def test_reserving_the_tick_only_ever_tightens():
    """Monotonicity. A reserve that could LOOSEN the bound would be worse
    than none: it would hand back room the rounding is about to consume."""
    plain = headroom_pct(2.0, 0.25, 0.0096)
    reserved = headroom_pct(2.0, 0.25, 0.0096, tick_size_frac=0.0001 / 0.15)
    assert reserved < plain


def test_a_tick_coarser_than_the_ring_leaves_no_room_at_all():
    """The degenerate market. If one tick is wider than the credit ring,
    there is no on-grid price inside it and the honest answer is zero --
    not a negative that would invert the retreat into an advance."""
    assert headroom_pct(2.0, 0.25, 0.0, tick_size_frac=0.5) == 0.0
    assert headroom_pct(2.0, 0.25, 0.0, tick_size_frac=0.02) == 0.0
