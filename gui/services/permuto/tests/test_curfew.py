"""[CURFEW] The inventory cap on a clock.

The measured facts these tests encode (sampled live 2026-08-30, US
equities closed): the venue reports trading_paused False and every market
active while all three oracles sit frozen to sixteen digits.  So the
schedule and the freeze detector -- not the venue's own flags -- are what
tell us the underlying is shut.
"""

from __future__ import annotations

from gui.services.permuto.curfew import (
    PREOPEN_EXIT_S,
    CLOSES_UTC,
    EXIT_START_S,
    OPENS_UTC,
    OVERNIGHT_LONG_FRACTION,
    FREEZE_CONFIRM_S,
    OVERNIGHT_SHORT_FRACTION,
    RAMP_START_S,
    SETTLE_AFTER_OPEN_S,
    OracleFreeze,
    Stage,
    assess_curfew,
    permitted_leg_size,
)

MON_OPEN = OPENS_UTC[0]        # 2026-08-31 13:30Z = 09:30 EDT
MON_CLOSE = CLOSES_UTC[0]      # 2026-08-31 20:00Z = 16:00 EDT
FULL = 1_200.0
NIGHT_LONG = FULL * OVERNIGHT_LONG_FRACTION     # 300
NIGHT_SHORT = FULL * OVERNIGHT_SHORT_FRACTION   # 120


def _at(now_s, frozen=False, full=FULL):
    return assess_curfew(now_s, full, frozen_oracle=frozen)


# --------------------------------------------------------------------------- #
# The session table
# --------------------------------------------------------------------------- #

def test_the_table_is_five_sessions_of_six_and_a_half_hours():
    assert len(CLOSES_UTC) == len(OPENS_UTC) == 5
    for open_s, close_s in zip(OPENS_UTC, CLOSES_UTC):
        assert close_s - open_s == 6.5 * 3600.0
    for i in range(1, 5):
        assert CLOSES_UTC[i] - CLOSES_UTC[i - 1] == 86_400.0


# --------------------------------------------------------------------------- #
# Stages through one session
# --------------------------------------------------------------------------- #

def test_before_the_first_open_the_market_is_closed_not_in_session():
    # Sunday evening: the venue is matching orders, the underlying is shut.
    state = _at(MON_OPEN - 3_600.0)
    assert state.stage is Stage.CLOSED
    assert (state.long_cap_usd, state.short_cap_usd) == (NIGHT_LONG,
                                                         NIGHT_SHORT)


def test_just_after_the_open_holds_the_reduced_size():
    state = _at(MON_OPEN + 60.0)
    assert state.stage is Stage.SETTLING
    assert state.long_cap_usd == NIGHT_LONG


def test_settling_is_two_sided_at_the_reduced_size():
    # [review] The curfew exists to stop us quoting against a STALE price.
    # By SETTLING the oracle is moving again, so that rationale is spent --
    # and holding the ask closed would forfeit depth credit (min(bid, ask))
    # through the busiest quarter-hour of the session.
    state = _at(MON_OPEN + 60.0)
    assert state.stage is Stage.SETTLING
    assert state.long_cap_usd == state.short_cap_usd == NIGHT_LONG


def test_mid_session_restores_the_full_cap_on_both_sides():
    state = _at(MON_OPEN + SETTLE_AFTER_OPEN_S + 60.0)
    assert state.stage is Stage.SESSION
    assert state.long_cap_usd == state.short_cap_usd == FULL


def test_the_last_minutes_hold_the_overnight_posture():
    state = _at(MON_CLOSE - 60.0)
    assert state.stage is Stage.EXIT
    assert (state.long_cap_usd, state.short_cap_usd) == (NIGHT_LONG,
                                                         NIGHT_SHORT)


def test_after_the_close_the_curfew_holds_all_night():
    # The exploit executes DURING the closed window, so this is the case
    # that matters most: hours after the bell, still capped.
    for hours in (0.5, 4.0, 12.0):
        state = _at(MON_CLOSE + hours * 3_600.0)
        assert state.stage is Stage.CLOSED
        assert (state.long_cap_usd, state.short_cap_usd) == (NIGHT_LONG,
                                                             NIGHT_SHORT)


# --------------------------------------------------------------------------- #
# [review] The constraint must never RELAX on the way into the close.
# The first version ramped one symmetric number to a floor and then handed
# EXIT a long cap of 25% of full, so the long allowance DOUBLED fifteen
# minutes before the bell and restarted inventory accumulation.
# --------------------------------------------------------------------------- #

def test_both_caps_fall_monotonically_from_the_ramp_into_the_night():
    longs, shorts = [], []
    # Ramp -> exit -> night only; SETTLING belongs to the NEXT session and
    # deliberately reopens the ask once the oracle is live again.
    t = MON_CLOSE - RAMP_START_S - 60.0        # a minute before the ramp
    while t <= MON_CLOSE + 3_600.0:
        state = _at(t)
        longs.append(state.long_cap_usd)
        shorts.append(state.short_cap_usd)
        t += 60.0
    assert longs == sorted(longs, reverse=True), "long cap increased"
    assert shorts == sorted(shorts, reverse=True), "short cap increased"
    assert longs[0] == FULL and longs[-1] == NIGHT_LONG
    assert shorts[0] == FULL and shorts[-1] == NIGHT_SHORT


def test_the_ramp_travels_most_of_the_way_rather_than_nudging():
    early = _at(MON_CLOSE - RAMP_START_S + 60.0)
    late = _at(MON_CLOSE - EXIT_START_S - 60.0)
    assert early.stage is late.stage is Stage.RAMP
    assert early.long_cap_usd - late.long_cap_usd > (FULL - NIGHT_LONG) * 0.8


def test_there_is_no_jump_across_the_ramp_exit_boundary():
    just_before = _at(MON_CLOSE - EXIT_START_S - 1.0)
    just_after = _at(MON_CLOSE - EXIT_START_S + 1.0)
    assert abs(just_before.long_cap_usd - just_after.long_cap_usd) < 1.0
    assert abs(just_before.short_cap_usd - just_after.short_cap_usd) < 1.0


# --------------------------------------------------------------------------- #
# Caps and the position limit
# --------------------------------------------------------------------------- #

def test_the_long_cap_is_always_strictly_positive():
    # risk.assess() reads a non-positive limit as "no limit"; the runner
    # clamps, but the long side must never be the thing that needs it.
    probes = [MON_OPEN - 86_400.0, MON_OPEN, MON_OPEN + 1.0,
              MON_CLOSE - RAMP_START_S, MON_CLOSE - EXIT_START_S,
              MON_CLOSE - 1.0, MON_CLOSE, MON_CLOSE + 50_000.0]
    for t in probes:
        for frozen in (True, False):
            assert _at(t, frozen=frozen).long_cap_usd > 0.0, (t, frozen)


def test_the_overnight_short_cap_is_small_but_not_zero():
    """[2026-08-31] It WAS exactly zero, and zero cost the whole night.

    Depth credit is min(bid, ask): an ask permitted at size zero makes
    min(bid, 0) = 0, so even a flat, fully funded account banked nothing
    overnight while five rivals compounded after hours. Small and
    non-zero makes a BALANCED book legal; the asymmetry with the long
    side is what still forbids a directional short.
    """
    state = _at(MON_CLOSE + 3_600.0)
    assert state.short_cap_usd == FULL * OVERNIGHT_SHORT_FRACTION
    assert 0.0 < state.short_cap_usd < state.long_cap_usd,         "the short side must stay strictly tighter than the long side"


def test_no_configured_limit_leaves_the_curfew_inactive():
    state = assess_curfew(MON_CLOSE - 60.0, 0.0, frozen_oracle=True)
    assert "no position limit" in state.reason


def test_cap_for_selects_by_the_position_we_actually_hold():
    state = _at(MON_CLOSE + 3_600.0)
    assert state.cap_for(10.0) == state.long_cap_usd
    assert state.cap_for(-10.0) == state.short_cap_usd
    assert state.cap_for(0.0) == max(state.long_cap_usd,
                                     state.short_cap_usd)
    assert state.cap_for(float("nan")) == min(state.long_cap_usd,
                                              state.short_cap_usd)


# --------------------------------------------------------------------------- #
# permitted_leg_size -- the blocker this replaced a boolean to fix
# --------------------------------------------------------------------------- #

def test_a_reducing_sell_is_clamped_to_the_long_it_closes():
    # THE BLOCKER. One contract of long inventory used to wave through a
    # full-size ask; filling it left us massively SHORT overnight, which is
    # the exact position this module exists to forbid. The sell may now be
    # exactly as large as the long, and not one contract more.
    assert permitted_leg_size(False, 1.0, 17_094.0, 4_285.0, 0.0) == 1.0
    assert permitted_leg_size(False, 250.0, 17_094.0, 4_285.0, 0.0) == 250.0


def test_a_zero_short_cap_refuses_the_ask_outright():
    # Contract of the FUNCTION given a zero cap -- no longer the overnight
    # configuration, which is now OVERNIGHT_SHORT_FRACTION > 0. Renamed so
    # the name stops implying a policy that changed underneath it.
    assert permitted_leg_size(False, 0.0, 17_094.0, 4_285.0, 0.0) == 0.0


def test_a_bid_is_clamped_to_the_long_cap():
    # And the clamp is what bounds it: the requested ladder leg is far
    # larger than the cap allows.
    assert permitted_leg_size(True, 0.0, 17_191.0, 4_285.0, 0.0) == 4_285.0


def test_a_buy_may_close_a_short_and_rebuild_to_the_long_cap():
    # Room is the distance from where we are to the cap, so reduction and
    # growth fall out of one expression.
    assert permitted_leg_size(True, -100.0, 99_999.0, 4_285.0, 0.0) == 4_385.0


def test_a_leg_smaller_than_the_room_is_untouched():
    assert permitted_leg_size(True, 0.0, 10.0, 4_285.0, 0.0) == 10.0
    assert permitted_leg_size(False, 500.0, 10.0, 4_285.0, 0.0) == 10.0


def test_no_room_means_no_leg():
    assert permitted_leg_size(True, 4_285.0, 100.0, 4_285.0, 0.0) == 0.0
    assert permitted_leg_size(True, 9_999.0, 100.0, 4_285.0, 0.0) == 0.0


def test_unreadable_or_empty_inventory_permits_nothing():
    nan = float("nan")
    assert permitted_leg_size(True, nan, 100.0, 10.0, 10.0) == 0.0
    assert permitted_leg_size(False, nan, 100.0, 10.0, 10.0) == 0.0
    assert permitted_leg_size(True, 0.0, nan, 10.0, 10.0) == 0.0
    assert permitted_leg_size(True, 0.0, 0.0, 10.0, 10.0) == 0.0
    assert permitted_leg_size(True, 0.0, -5.0, 10.0, 10.0) == 0.0


def test_mid_session_the_clamp_is_just_a_position_limit():
    # With both caps equal the asymmetry vanishes and this degrades to the
    # ordinary limit -- the lean belongs to the closed window, not to the
    # strategy.
    assert permitted_leg_size(False, 0.0, 100.0, 4_285.0, 4_285.0) == 100.0
    assert permitted_leg_size(True, 0.0, 100.0, 4_285.0, 4_285.0) == 100.0


# --------------------------------------------------------------------------- #
# The freeze detector
# --------------------------------------------------------------------------- #

def test_a_fresh_detector_does_not_claim_frozen():
    assert OracleFreeze().frozen(1_000.0) is False


def test_a_moving_oracle_is_never_frozen():
    f = OracleFreeze(confirm_s=100.0)
    t = 0.0
    for i in range(10):
        f.observe(t, {"QQQ-VOL-PERP": 0.15 + i * 1e-9})
        t += 50.0
        assert f.frozen(t) is False


def test_an_unchanging_oracle_freezes_after_the_confirm_window():
    f = OracleFreeze(confirm_s=100.0)
    f.observe(0.0, {"QQQ-VOL-PERP": 0.1544391445921985})
    f.observe(50.0, {"QQQ-VOL-PERP": 0.1544391445921985})
    assert f.frozen(50.0) is False
    assert f.frozen(100.0) is True
    assert f.frozen(5_000.0) is True


def test_one_market_moving_is_enough_to_count_as_alive():
    f = OracleFreeze(confirm_s=100.0)
    f.observe(0.0, {"A": 1.0, "B": 2.0})
    f.observe(150.0, {"A": 1.0, "B": 2.000001})
    assert f.frozen(150.0) is False


def test_an_empty_reading_is_not_evidence_of_a_freeze():
    f = OracleFreeze(confirm_s=100.0)
    f.observe(0.0, {"A": 1.0})
    f.observe(10.0, {})
    f.observe(20.0, {})
    assert f.frozen(50.0) is False
    assert f.frozen(100.0) is True          # an outage outlasting it tightens


def test_a_resumed_move_after_a_freeze_clears_it():
    f = OracleFreeze(confirm_s=100.0)
    f.observe(0.0, {"A": 1.0})
    assert f.frozen(200.0) is True
    f.observe(210.0, {"A": 1.5})
    assert f.frozen(210.0) is False


# --------------------------------------------------------------------------- #
# The asymmetric combination rule
# --------------------------------------------------------------------------- #

def test_a_frozen_oracle_overrides_an_in_session_schedule():
    mid = MON_OPEN + 3 * 3_600.0
    assert _at(mid).stage is Stage.SESSION
    frozen = _at(mid, frozen=True)
    assert frozen.stage is Stage.CLOSED
    assert frozen.short_cap_usd == FULL * OVERNIGHT_SHORT_FRACTION


def test_a_moving_oracle_does_not_lift_a_scheduled_close():
    state = _at(MON_CLOSE + 3_600.0, frozen=False)
    assert state.stage is Stage.CLOSED
    assert state.short_cap_usd == FULL * OVERNIGHT_SHORT_FRACTION


def test_a_frozen_oracle_never_raises_either_cap():
    for t in (MON_OPEN - 600.0, MON_OPEN + 60.0, MON_OPEN + 3 * 3_600.0,
              MON_CLOSE - 1_000.0, MON_CLOSE + 600.0):
        frozen, moving = _at(t, frozen=True), _at(t, frozen=False)
        assert frozen.long_cap_usd <= moving.long_cap_usd
        assert frozen.short_cap_usd <= moving.short_cap_usd


# --------------------------------------------------------------------------- #
# Past the end of the table
# --------------------------------------------------------------------------- #

def test_past_the_table_a_moving_market_trades_normally():
    after = CLOSES_UTC[-1] + 7 * 86_400.0
    state = _at(after, frozen=False)
    assert state.stage is Stage.UNSCHEDULED
    assert state.long_cap_usd == state.short_cap_usd == FULL


def test_past_the_table_a_frozen_market_is_still_a_curfew():
    after = CLOSES_UTC[-1] + 7 * 86_400.0
    state = _at(after, frozen=True)
    assert state.stage is Stage.CLOSED
    assert (state.long_cap_usd, state.short_cap_usd) == (NIGHT_LONG,
                                                         NIGHT_SHORT)


def test_every_state_carries_a_reason():
    for t in (MON_OPEN - 600.0, MON_OPEN + 60.0, MON_OPEN + 3 * 3_600.0,
              MON_CLOSE - 3_000.0, MON_CLOSE - 60.0, MON_CLOSE + 600.0,
              CLOSES_UTC[-1] + 86_400.0):
        for frozen in (True, False):
            assert _at(t, frozen=frozen).reason.strip()


# --------------------------------------------------------------------------- #
# PREOPEN -- the retraction PR #130 claimed but did not have
# --------------------------------------------------------------------------- #

def test_the_short_side_is_shut_before_the_bell():
    """The gap arrives AT the open, and it fills whatever is resting.

    [review 2026-08-31] PR #130's comment claimed EXIT/SETTLING retract
    before the open. They do not: SETTLING is gated on since_open >= 0, so
    it begins AFTER the bell and the whole night is CLOSED. With a non-zero
    overnight short cap the ask rested straight through the opening jump
    and was swept BY it -- filled short at pre-gap prices into a +73% to
    +229% move, which is exactly how this account lost $523k of unrealised
    P&L on night one.
    """
    state = _at(MON_OPEN - 600.0)
    assert state.stage is Stage.PREOPEN
    assert state.short_cap_usd == 0.0, "an ask can still rest into the gap"


def test_the_long_side_survives_the_pre_open_window():
    # Long into an upward vol gap is the harmless side, and shutting it too
    # would forfeit the tail of the cheapest depth window for no gain.
    assert _at(MON_OPEN - 600.0).long_cap_usd > 0.0


def test_the_window_starts_exactly_where_it_says():
    assert _at(MON_OPEN - PREOPEN_EXIT_S + 1.0).stage is Stage.PREOPEN
    assert _at(MON_OPEN - PREOPEN_EXIT_S - 60.0).stage is Stage.CLOSED


def test_earlier_in_the_night_the_short_side_is_open_for_depth():
    """The whole point of the change: most of the night still earns."""
    mid = MON_CLOSE + 3_600.0
    state = _at(mid)
    assert state.stage is Stage.CLOSED
    assert state.short_cap_usd == FULL * OVERNIGHT_SHORT_FRACTION > 0.0


def test_a_frozen_oracle_cannot_reopen_the_short_before_the_bell():
    """A frozen oracle may only ever TIGHTEN.

    Collapsing PREOPEN to CLOSED raised the short cap from 0 to the
    overnight fraction in the last half hour -- a freeze re-opening the
    exact side the window exists to shut.
    """
    frozen = _at(MON_OPEN - 600.0, frozen=True)
    assert frozen.short_cap_usd == 0.0
    assert frozen.short_cap_usd <= _at(MON_OPEN - 600.0, frozen=False).short_cap_usd


def test_a_frozen_oracle_just_after_the_open_keeps_the_short_shut():
    """[review 2026-08-31] The second version of the same hole.

    The first tick at/after the bell leaves PREOPEN for SETTLING. If the
    oracle has not printed yet, mapping that to CLOSED restores the
    overnight short cap and lets a fresh ask rest against a price that is
    still stale -- re-opening the exposure PREOPEN spent the previous half
    hour preventing, at the exact moment the gap is most likely to land.
    A frozen oracle after the open means the session has NOT started.
    """
    just_after = MON_OPEN + 60.0
    assert _at(just_after, frozen=False).stage is Stage.SETTLING
    frozen = _at(just_after, frozen=True)
    assert frozen.short_cap_usd == 0.0,         "a frozen oracle after the bell reopened the short side"
    assert frozen.short_cap_usd <= _at(just_after, frozen=False).short_cap_usd


def test_the_frozen_posture_lifts_once_the_oracle_prints():
    """And it must not be a one-way latch: a moving oracle after the open
    is a real session, and holding zero-short there would forfeit the
    busiest quarter hour for a danger that has passed."""
    moving = _at(MON_OPEN + 60.0, frozen=False)
    assert moving.stage is Stage.SETTLING
    assert moving.short_cap_usd > 0.0


def test_a_first_sighting_is_not_a_print():
    """[review] Cold start during SETTLING.

    Seeding used to go through the inequality -- None != value -- and so
    stamped the market as having just moved. A runner started after the
    bell then read a frozen pre-open oracle as FRESH for the whole
    confirm_s window and quoted against it, which is exactly the
    stale-price case the freshness gate exists to catch.
    """
    f = OracleFreeze()
    f.observe(1000.0, {"QQQ-VOL-PERP": 0.07})
    assert f.market_frozen("QQQ-VOL-PERP", 1000.0) is True,         "a first sighting was treated as evidence the market is printing"
    # Still frozen a moment later: nothing has moved.
    assert f.market_frozen("QQQ-VOL-PERP", 1060.0) is True


def test_an_actual_change_is_what_marks_a_market_fresh():
    f = OracleFreeze()
    f.observe(1000.0, {"QQQ-VOL-PERP": 0.07})
    f.observe(1010.0, {"QQQ-VOL-PERP": 0.08})        # a real print
    assert f.market_frozen("QQQ-VOL-PERP", 1010.0) is False
    # And it goes stale again once confirm_s passes with no further move.
    assert f.market_frozen("QQQ-VOL-PERP", 1010.0 + FREEZE_CONFIRM_S) is True


def test_an_unseen_market_is_frozen_not_fresh():
    """Unknown freshness must tighten: never having seen a market move is
    not evidence that it does."""
    assert OracleFreeze().market_frozen("NEVER-SEEN", 1000.0) is True


def test_a_market_frozen_since_startup_eventually_goes_quiet():
    """[review] The fail-open that kept a dead feed quotable forever.

    _changed_at_by_market_s is only written on an actual change, and a
    first sighting deliberately does not count as one. So a market that
    was ALREADY frozen when the runner started never landed in that dict
    at all -- and market_gone_quiet() answered False on the missing key,
    which meant "still fresh", which meant quotable. Not just during the
    confirmation window: forever.

    That is precisely the market this gate exists to catch. The aggregate
    detector cannot see it, because it resets whenever ANY market moves,
    so a neighbour ticking along keeps the whole curfew in SESSION while
    this one sits on a price from before the process started.
    """
    f = OracleFreeze(confirm_s=100.0)
    f.observe(1000.0, {"QQQ-VOL-PERP": 0.07})
    # Inside the window it is still absence of evidence -- we have not
    # been watching long enough to call it.
    assert f.market_gone_quiet("QQQ-VOL-PERP", 1050.0) is False
    # Keep reading the same value. At confirm_s, sitting still IS the
    # evidence.
    f.observe(1050.0, {"QQQ-VOL-PERP": 0.07})
    f.observe(1100.0, {"QQQ-VOL-PERP": 0.07})
    assert f.market_gone_quiet("QQQ-VOL-PERP", 1100.0) is True,         "a market that has not moved since startup stayed quotable forever"


def test_a_market_we_have_never_observed_is_not_reported_quiet():
    """The other half of the same call, and the reason it cannot simply
    return True on a missing key: never having looked is not the same as
    having watched it sit still. market_frozen() is the one that tightens
    on an unseen market; this one must not, or the runner refuses to quote
    anything until a second distinct value arrives.
    """
    f = OracleFreeze(confirm_s=100.0)
    assert f.market_gone_quiet("NEVER-SEEN", 10_000.0) is False
    # Even long after a different market has been seen and gone quiet.
    f.observe(1000.0, {"QQQ-VOL-PERP": 0.07})
    f.observe(1200.0, {"QQQ-VOL-PERP": 0.07})
    assert f.market_gone_quiet("QQQ-VOL-PERP", 1200.0) is True
    assert f.market_gone_quiet("NEVER-SEEN", 1200.0) is False


def test_a_late_print_refreshes_a_market_that_had_gone_quiet():
    """The first-seen fallback must not outrank a real print -- otherwise
    a market that woke up would stay branded quiet on the strength of when
    we happened to start watching it."""
    f = OracleFreeze(confirm_s=100.0)
    f.observe(1000.0, {"QQQ-VOL-PERP": 0.07})
    f.observe(1200.0, {"QQQ-VOL-PERP": 0.07})
    assert f.market_gone_quiet("QQQ-VOL-PERP", 1200.0) is True
    f.observe(1210.0, {"QQQ-VOL-PERP": 0.09})        # it wakes up
    assert f.market_gone_quiet("QQQ-VOL-PERP", 1210.0) is False
    # ...and goes quiet again on its own schedule, not the startup one.
    assert f.market_gone_quiet("QQQ-VOL-PERP", 1310.0) is True


def test_a_print_landing_exactly_on_the_bell_counts_as_after_it():
    """[review] A strict `>` rejected the opening print itself.

    The boundaries here are exact round timestamps and the loop ticks on a
    timer, so a print stamped at precisely the open is not an exotic edge
    -- and it is the very print SETTLING is waiting for. Rejecting it held
    the stage in the stale-withdraw posture until some later change
    happened along.
    """
    f = OracleFreeze()
    f.observe(999.0, {"QQQ-VOL-PERP": 0.07})     # first sighting: no stamp
    f.observe(1000.0, {"QQQ-VOL-PERP": 0.08})    # the opening print, at the bell
    assert f.changed_since("QQQ-VOL-PERP", 1000.0) is True,         "the opening print was not accepted as having happened at the open"
    # A print BEFORE the bell still does not count.
    g = OracleFreeze()
    g.observe(900.0, {"QQQ-VOL-PERP": 0.07})
    g.observe(999.0, {"QQQ-VOL-PERP": 0.08})
    assert g.changed_since("QQQ-VOL-PERP", 1000.0) is False
