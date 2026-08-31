"""Per-stage quoting profiles.

The properties worth pinning are the ones that encode measurements rather
than taste: the overnight window must be the one that quotes largest, the
close must stop adding inventory, and a frozen oracle after the bell must
not be mistaken for a session starting.
"""

from gui.services.permuto.curfew import Stage
from gui.services.permuto.modes import (
    CLOSED_SPREAD_MULT, SESSION_SPREAD_MULT, Profile, profile_for,
)


def test_every_stage_has_a_profile():
    for stage in list(Stage):
        p = profile_for(stage)
        assert isinstance(p, Profile)
        assert p.reason, "%s has no stated reason" % stage


def test_the_overnight_window_quotes_the_largest():
    """The measurement this whole module exists for.

    A frozen oracle is a fixed ring: quotes rest without drift, without
    band rejections, and without anything to be picked off by. The leader
    banked 4.2M -> 216M depth-seconds there overnight while the cash
    session produced 53 refusals. So CLOSED must be the biggest, not a
    cautious fraction of the session size.
    """
    closed = profile_for(Stage.CLOSED)
    for other in (Stage.SESSION, Stage.RAMP, Stage.EXIT, Stage.PREOPEN,
                  Stage.SETTLING):
        assert closed.depth_mult >= profile_for(other).depth_mult, \
            "%s quotes at least as large as the overnight window" % other


def test_open_hours_quote_wider_than_overnight():
    """20-24% median one-minute moves: a tight quote is a donation, and
    depth credit is flat inside the ring so width is free."""
    assert profile_for(Stage.SESSION).spread_mult > \
        profile_for(Stage.CLOSED).spread_mult
    assert profile_for(Stage.CLOSED).spread_mult == CLOSED_SPREAD_MULT
    assert profile_for(Stage.SESSION).spread_mult == SESSION_SPREAD_MULT


def test_the_last_minutes_place_nothing_new():
    p = profile_for(Stage.EXIT)
    assert p.quote is False
    assert p.depth_mult == 0.0


def test_the_close_run_in_stops_adding_inventory():
    """Inventory carried past the bell is a claim on the overnight window.

    870k contracts short at the close pinned every market to reduce-only
    and earned zero through the most valuable hours of the week. What this
    module does about it is bounded: RAMP shrinks and EXIT stops. It does
    NOT actively skew to shed what is already on -- an earlier draft had a
    `flatten_bias` flag that nothing read, describing behaviour the code
    did not have.
    """
    assert (profile_for(Stage.RAMP).depth_mult
            < profile_for(Stage.SESSION).depth_mult)
    assert profile_for(Stage.EXIT).depth_mult == 0.0
    assert profile_for(Stage.EXIT).quote is False


def test_size_shrinks_monotonically_into_the_close():
    assert (profile_for(Stage.SESSION).depth_mult
            > profile_for(Stage.RAMP).depth_mult
            > profile_for(Stage.EXIT).depth_mult)


def test_a_frozen_oracle_after_the_bell_stops_quoting():
    """The schedule says the session began; the price says it has not.

    Quoting against that hands a free option to anyone who can see the
    real underlying -- and at the open the oracle gapped +73% to +229% in
    a single print.
    """
    stale = profile_for(Stage.SETTLING, oracle_fresh=False)
    assert stale.quote is False
    assert stale.depth_mult == 0.0


def test_a_printing_oracle_after_the_bell_does_quote():
    """And it must not latch: once the price moves, the session is real
    and holding out forfeits the busiest quarter hour for nothing."""
    live = profile_for(Stage.SETTLING, oracle_fresh=True)
    assert live.quote is True
    assert live.depth_mult > 0.0


def test_freshness_only_matters_where_it_can_mislead():
    """CLOSED is EXPECTED to be frozen -- that is the whole opportunity --
    so a frozen oracle must not shut the overnight window down."""
    assert profile_for(Stage.CLOSED, oracle_fresh=False).quote is True
    assert profile_for(Stage.CLOSED, oracle_fresh=False).depth_mult == \
        profile_for(Stage.CLOSED, oracle_fresh=True).depth_mult


def test_no_profile_can_widen_past_the_credit_ring_at_defaults():
    """A profile that quotes outside the ring earns nothing, so the
    multipliers have to stay usable at the shipped spread."""
    half_spread, ring = 0.25, 2.0
    for stage in list(Stage):
        p = profile_for(stage)
        assert half_spread * p.spread_mult < ring, (
            "%s widens 0.25%% to %.2f%%, at or past the %.1f%% ring"
            % (stage, half_spread * p.spread_mult, ring))
