"""[CURFEW] The inventory cap on a clock.

The measured facts these tests encode (sampled live 2026-08-30, US
equities closed): the venue reports trading_paused False and every market
active while all three oracles sit frozen to sixteen digits.  So the
schedule and the freeze detector -- not the venue's own flags -- are what
tell us the underlying is shut.
"""

from __future__ import annotations

from gui.services.permuto.curfew import (
    OVERNIGHT_LONG_FRACTION,
    OVERNIGHT_SHORT_FRACTION,
    leg_permitted,
    CLOSES_UTC,
    EXIT_START_S,
    FLOOR_FRACTION,
    OPENS_UTC,
    RAMP_START_S,
    SETTLE_AFTER_OPEN_S,
    OracleFreeze,
    Stage,
    assess_curfew,
)

MON_OPEN = OPENS_UTC[0]        # 2026-08-31 13:30Z = 09:30 EDT
MON_CLOSE = CLOSES_UTC[0]      # 2026-08-31 20:00Z = 16:00 EDT
FULL = 1_200.0
FLOOR = FULL * FLOOR_FRACTION


def _at(now_s, frozen=False, full=FULL):
    return assess_curfew(now_s, full, frozen_oracle=frozen)


# --------------------------------------------------------------------------- #
# The table itself
# --------------------------------------------------------------------------- #

def test_the_table_is_five_sessions_of_six_and_a_half_hours():
    assert len(CLOSES_UTC) == len(OPENS_UTC) == 5
    for open_s, close_s in zip(OPENS_UTC, CLOSES_UTC):
        assert close_s - open_s == 6.5 * 3600.0
    # Consecutive weekdays, one session per day.
    for i in range(1, 5):
        assert CLOSES_UTC[i] - CLOSES_UTC[i - 1] == 86_400.0


# --------------------------------------------------------------------------- #
# Stages through one session
# --------------------------------------------------------------------------- #

def test_before_the_first_open_the_market_is_closed_not_in_session():
    # Sunday evening: the venue is matching orders, the underlying is shut.
    state = _at(MON_OPEN - 3_600.0)
    assert state.stage is Stage.CLOSED
    assert state.cap_usd == FLOOR


def test_just_after_the_open_is_settling_at_the_floor():
    state = _at(MON_OPEN + 60.0)
    assert state.stage is Stage.SETTLING
    assert state.cap_usd == FLOOR


def test_mid_session_restores_the_full_cap():
    state = _at(MON_OPEN + SETTLE_AFTER_OPEN_S + 60.0)
    assert state.stage is Stage.SESSION
    assert state.cap_usd == FULL


def test_the_ramp_shrinks_the_cap_monotonically_toward_the_floor():
    caps = []
    t = MON_CLOSE - RAMP_START_S
    # Stop strictly BEFORE the EXIT boundary: at exactly EXIT_START_S the
    # stage is EXIT by design (the comparison there is inclusive).
    while t < MON_CLOSE - EXIT_START_S:
        state = _at(t)
        assert state.stage is Stage.RAMP
        caps.append(state.cap_usd)
        t += 300.0
    assert caps == sorted(caps, reverse=True)      # never increases
    assert caps[0] <= FULL
    assert caps[-1] >= FLOOR
    # It actually travels most of the way, rather than nudging.
    assert caps[0] - caps[-1] > (FULL - FLOOR) * 0.8


def test_the_last_minutes_pin_the_floor():
    state = _at(MON_CLOSE - 60.0)
    assert state.stage is Stage.EXIT
    assert state.cap_usd == FLOOR


def test_after_the_close_the_curfew_holds_all_night():
    # The exploit executes DURING the closed window, so this is the case
    # that matters most: hours after the bell, still floored.
    for hours in (0.5, 4.0, 12.0):
        state = _at(MON_CLOSE + hours * 3_600.0)
        assert state.stage is Stage.CLOSED
        assert state.cap_usd == FLOOR


# --------------------------------------------------------------------------- #
# The floor is never zero -- risk.assess() reads a non-positive limit as
# "no limit", so a ramp to zero would restore UNLIMITED inventory.
# --------------------------------------------------------------------------- #

def test_the_cap_is_always_strictly_positive():
    probes = [MON_OPEN - 86_400.0, MON_OPEN, MON_OPEN + 1.0,
              MON_CLOSE - RAMP_START_S, MON_CLOSE - EXIT_START_S,
              MON_CLOSE - 1.0, MON_CLOSE, MON_CLOSE + 50_000.0]
    for t in probes:
        for frozen in (True, False):
            assert _at(t, frozen=frozen).cap_usd > 0.0, (t, frozen)


def test_an_explicit_zero_floor_is_refused():
    state = assess_curfew(MON_CLOSE - 60.0, FULL, frozen_oracle=False,
                          floor_usd=0.0)
    assert state.cap_usd > 0.0


def test_the_cap_never_exceeds_the_operators_limit():
    for t in (MON_OPEN + 3_600.0, MON_CLOSE - RAMP_START_S + 1.0):
        assert _at(t).cap_usd <= FULL


def test_no_configured_limit_leaves_the_curfew_inactive():
    # 0 means "no limit" downstream; inventing one would be the curfew
    # imposing a constraint the operator never set.
    state = assess_curfew(MON_CLOSE - 60.0, 0.0, frozen_oracle=True)
    assert state.cap_usd == 0.0
    assert "no position limit" in state.reason


# --------------------------------------------------------------------------- #
# The freeze detector
# --------------------------------------------------------------------------- #

def test_a_fresh_detector_does_not_claim_frozen():
    # A process that has not looked yet must not declare the market shut.
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
    # A venue we cannot read tells us nothing about the underlying; it must
    # not reset the clock either way.
    f = OracleFreeze(confirm_s=100.0)
    f.observe(0.0, {"A": 1.0})
    f.observe(10.0, {})
    f.observe(20.0, {})
    assert f.frozen(50.0) is False          # still inside the window
    assert f.frozen(100.0) is True          # outage outlasting it tightens


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
    # The clock wrong in the direction that costs money: the table says
    # mid-session, the price has stopped. Tighten.
    mid = MON_OPEN + 3 * 3_600.0
    assert _at(mid).stage is Stage.SESSION
    frozen = _at(mid, frozen=True)
    assert frozen.stage is Stage.CLOSED
    assert frozen.cap_usd == FLOOR


def test_a_moving_oracle_does_not_lift_a_scheduled_close():
    # Lifting needs BOTH conditions, so a stray overnight print cannot
    # release the curfew.
    state = _at(MON_CLOSE + 3_600.0, frozen=False)
    assert state.stage is Stage.CLOSED
    assert state.cap_usd == FLOOR


def test_a_frozen_oracle_cannot_raise_the_cap_anywhere():
    for t in (MON_OPEN - 600.0, MON_OPEN + 60.0, MON_OPEN + 3 * 3_600.0,
              MON_CLOSE - 1_000.0, MON_CLOSE + 600.0):
        assert _at(t, frozen=True).cap_usd <= _at(t, frozen=False).cap_usd


# --------------------------------------------------------------------------- #
# Past the end of the table
# --------------------------------------------------------------------------- #

def test_past_the_table_a_moving_market_trades_normally():
    # The table expires at the contest end; degrading to a permanent floor
    # would quietly cripple the bot, and degrading to "no curfew" would
    # quietly remove the protection. Fall back to the observable truth.
    after = CLOSES_UTC[-1] + 7 * 86_400.0
    state = _at(after, frozen=False)
    assert state.stage is Stage.UNSCHEDULED
    assert state.cap_usd == FULL


def test_past_the_table_a_frozen_market_is_still_a_curfew():
    after = CLOSES_UTC[-1] + 7 * 86_400.0
    state = _at(after, frozen=True)
    assert state.stage is Stage.CLOSED
    assert state.cap_usd == FLOOR


# --------------------------------------------------------------------------- #
# Every stage explains itself -- these strings go in front of an operator
# at 16:00 on a contest day.
# --------------------------------------------------------------------------- #

def test_every_state_carries_a_reason():
    for t in (MON_OPEN - 600.0, MON_OPEN + 60.0, MON_OPEN + 3 * 3_600.0,
              MON_CLOSE - 3_000.0, MON_CLOSE - 60.0, MON_CLOSE + 600.0,
              CLOSES_UTC[-1] + 86_400.0):
        for frozen in (True, False):
            assert _at(t, frozen=frozen).reason.strip()


# --------------------------------------------------------------------------- #
# Side-aware caps. The oracle freezes on a calm end-of-day window and the
# first print after the reopen comes from the most violent minute of the
# day, so it is systematically HIGHER -- which makes a carried SHORT the
# position that gets liquidated. The curfew is tighter on that side.
# --------------------------------------------------------------------------- #

def test_overnight_no_new_shorts_but_a_bounded_long():
    state = _at(MON_CLOSE + 4 * 3_600.0)
    assert state.short_cap_usd == 0.0 == FULL * OVERNIGHT_SHORT_FRACTION
    assert state.long_cap_usd == FULL * OVERNIGHT_LONG_FRACTION
    assert state.short_cap_usd < state.long_cap_usd


def test_a_deliberate_zero_short_cap_survives_construction():
    # Zero is a real value here, not "unset". If __post_init__ treated it
    # as missing it would silently become the symmetric cap and the
    # prohibition would vanish.
    state = _at(MON_CLOSE + 4 * 3_600.0)
    assert state.short_cap_usd == 0.0
    assert state.cap_usd == FLOOR          # the named cap is unchanged


def test_mid_session_the_sides_are_symmetric():
    # The asymmetry is a property of the CLOSED window, not a permanent
    # directional lean.
    state = _at(MON_OPEN + 3 * 3_600.0)
    assert state.long_cap_usd == state.short_cap_usd == FULL


def test_the_symmetric_cap_still_reports_the_tightest_constraint():
    # cap_usd is what the stage is named for, and existing callers read it.
    state = _at(MON_CLOSE + 3_600.0)
    assert state.cap_usd == FLOOR


def test_cap_for_selects_by_the_position_we_actually_hold():
    state = _at(MON_CLOSE + 3_600.0)
    assert state.cap_for(10.0) == state.long_cap_usd
    assert state.cap_for(-10.0) == state.short_cap_usd == 0.0
    # Flat takes the looser side; the per-leg veto, not this number, is
    # what stops the dangerous side being grown from zero.
    assert state.cap_for(0.0) == max(state.long_cap_usd,
                                     state.short_cap_usd)
    assert state.cap_for(float("nan")) == min(state.long_cap_usd,
                                              state.short_cap_usd)


def test_overnight_from_flat_only_the_bid_may_open():
    # THE POINT OF THE WHOLE FEATURE, in one assertion: at exactly zero
    # neither side "shrinks" and no position limit is breached, so a
    # symmetric cap would happily open a short into the frozen oracle.
    state = _at(MON_CLOSE + 4 * 3_600.0)
    oracle = 0.07
    long_c = state.long_cap_usd / oracle
    short_c = state.short_cap_usd / oracle
    assert leg_permitted(True, 0.0, long_c, short_c) is True     # bid opens
    assert leg_permitted(False, 0.0, long_c, short_c) is False   # ask does not


def test_an_existing_short_can_always_be_worked_off():
    # The prohibition is on GROWING the short, never on reducing one that
    # was carried in from the session.
    state = _at(MON_CLOSE + 4 * 3_600.0)
    oracle = 0.07
    long_c = state.long_cap_usd / oracle
    short_c = state.short_cap_usd / oracle
    assert leg_permitted(True, -25.0, long_c, short_c) is True   # buy reduces
    assert leg_permitted(False, -25.0, long_c, short_c) is False


def test_the_long_cap_is_never_below_the_floor():
    # Otherwise the "safe" side would be tighter than the dangerous one.
    for full in (10.0, 100.0, 1_200.0, 50_000.0):
        state = assess_curfew(MON_CLOSE + 3_600.0, full, frozen_oracle=True)
        assert state.long_cap_usd >= state.short_cap_usd


# --------------------------------------------------------------------------- #
# leg_permitted: the veto that a symmetric cap cannot express
# --------------------------------------------------------------------------- #

def test_a_reducing_leg_is_always_permitted():
    # Nothing may stand between the loop and getting smaller -- even with
    # both caps already breached.
    assert leg_permitted(True, -50.0, 0.1, 0.1) is True     # buy cuts short
    assert leg_permitted(False, 50.0, 0.1, 0.1) is True     # sell cuts long


def test_growing_a_side_stops_at_that_sides_cap():
    assert leg_permitted(True, 5.0, 10.0, 10.0) is True     # long 5 < 10
    assert leg_permitted(True, 10.0, 10.0, 10.0) is False   # at the cap
    assert leg_permitted(False, -5.0, 10.0, 10.0) is True
    assert leg_permitted(False, -10.0, 10.0, 10.0) is False


def test_from_flat_the_asymmetry_decides_which_side_may_open():
    # THE CASE A SYMMETRIC CAP CANNOT EXPRESS: at exactly zero neither side
    # "shrinks", so REDUCE_ONLY says nothing and the position limit is not
    # breached -- yet the short side must stay small.
    long_cap, short_cap = 100.0, 1.0
    assert leg_permitted(True, 0.0, long_cap, short_cap) is True
    assert leg_permitted(False, 0.0, long_cap, short_cap) is True   # 0 < 1
    # Once the tiny short allowance is used up, selling stops while buying
    # continues.
    assert leg_permitted(False, -1.0, long_cap, short_cap) is False
    assert leg_permitted(True, -1.0, long_cap, short_cap) is True


def test_unreadable_inventory_permits_nothing():
    nan = float("nan")
    assert leg_permitted(True, nan, 10.0, 10.0) is False
    assert leg_permitted(False, nan, 10.0, 10.0) is False
