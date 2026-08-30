"""[CURFEW] The inventory cap on a clock.

The measured facts these tests encode (sampled live 2026-08-30, US
equities closed): the venue reports trading_paused False and every market
active while all three oracles sit frozen to sixteen digits.  So the
schedule and the freeze detector -- not the venue's own flags -- are what
tell us the underlying is shut.
"""

from __future__ import annotations

from gui.services.permuto.curfew import (
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
