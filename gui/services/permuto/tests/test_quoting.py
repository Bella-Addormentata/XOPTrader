"""Quoting-loop decisions.

Ordered the way the loop is: the states that make quoting UNSAFE first, then
the ones that make it merely unnecessary. Getting that order wrong is how a
bot keeps quoting through a pause, or keeps a stale-priced book resting
because it looks two-sided.
"""

from __future__ import annotations

import pytest

from gui.services.permuto.quoting import (
    MAX_ORACLE_AGE_S,
    REQUOTE_AT_RING_FRACTION,
    LoopAction,
    RestingQuote,
    VenueView,
    decide,
)

ORACLE = 0.10
GOOD = RestingQuote(bid_price=0.0999, ask_price=0.1001)


def view(**kw):
    base = dict(oracle=ORACLE, oracle_age_s=1.0, session_ok=True)
    base.update(kw)
    return VenueView(**base)


# --------------------------------------------------------------------------- #
# Unsafe beats unnecessary
# --------------------------------------------------------------------------- #

def test_a_paused_venue_takes_the_book_down():
    """The one thing the sponsor said bots must handle -- and the Sunday reset
    happens inside a pause, so every entrant passes through this state."""
    d = decide(view(trading_paused=True), GOOD)
    assert d.action is LoopAction.WITHDRAW
    assert "paused" in d.reason


def test_pause_outranks_everything_including_a_missing_side():
    """A lifted side is urgent, but not more urgent than not fighting rejects."""
    d = decide(view(trading_paused=True), RestingQuote(bid_price=0.0999))
    assert d.action is LoopAction.WITHDRAW


def test_no_session_withdraws_rather_than_quoting_into_a_401():
    assert decide(view(session_ok=False), GOOD).action is LoopAction.WITHDRAW


def test_a_session_backoff_waits_instead_of_withdrawing():
    """Distinct from WITHDRAW: the book may still be legitimately resting and
    tearing it down for a transient renewal failure costs accrual."""
    d = decide(view(session_ok=False, session_waiting=True), GOOD)
    assert d.action is LoopAction.WAIT


def test_a_stale_oracle_withdraws_because_the_failure_is_invisible():
    """Quoting against a stale copy produces orders that look right locally,
    are rejected or purged by the venue, and score nothing -- with nothing on
    our side reporting it."""
    d = decide(view(oracle_age_s=MAX_ORACLE_AGE_S + 0.1), GOOD)
    assert d.action is LoopAction.WITHDRAW
    assert "stale" in d.reason


def test_a_fresh_enough_oracle_is_fine():
    assert decide(view(oracle_age_s=MAX_ORACLE_AGE_S), GOOD).action is LoopAction.HOLD


def test_a_missing_oracle_is_not_treated_as_permission():
    for bad in (None, 0.0, -1.0):
        assert decide(view(oracle=bad), GOOD).action is LoopAction.WITHDRAW


def test_carried_quoting_can_be_switched_off():
    """8x stressed initial margin, and entrants demonstrably hunt resting size
    out of hours -- so this is a policy an operator may want."""
    assert decide(view(carried=True), GOOD,
                  quote_when_carried=False).action is LoopAction.WITHDRAW
    assert decide(view(carried=True), GOOD,
                  quote_when_carried=True).action is LoopAction.HOLD


# --------------------------------------------------------------------------- #
# The open cancels everything
# --------------------------------------------------------------------------- #

def test_a_reopen_requotes_even_though_our_book_looks_fine():
    """The sequencer cancels ALL resting orders at the open. Believing our own
    state here means quoting nothing through the busiest hour of the day."""
    d = decide(view(just_reopened=True), GOOD)
    assert d.action is LoopAction.QUOTE
    assert "reopened" in d.reason


def test_a_reopen_while_paused_still_withdraws():
    """Unsafe still outranks stale-state."""
    d = decide(view(just_reopened=True, trading_paused=True), GOOD)
    assert d.action is LoopAction.WITHDRAW


# --------------------------------------------------------------------------- #
# min(bid, ask) is the whole game
# --------------------------------------------------------------------------- #

def test_a_lifted_side_is_a_quote_not_a_hold():
    """Credit is the minimum, so one missing side means this market earns
    ZERO -- restoring it is not tidying up."""
    d = decide(view(), RestingQuote(bid_price=0.0999))
    assert d.action is LoopAction.QUOTE
    assert "ZERO" in d.reason


def test_both_sides_lifted_also_requotes():
    assert decide(view(), RestingQuote()).action is LoopAction.QUOTE


# --------------------------------------------------------------------------- #
# Drift
# --------------------------------------------------------------------------- #

def test_a_quote_inside_the_ring_is_left_alone():
    """A refresh costs a mutate token and empties the book between states."""
    assert decide(view(), GOOD).action is LoopAction.HOLD


def test_drift_past_the_trigger_requotes_before_the_edge():
    """An order that reaches the ring boundary has already spent time earning
    nothing, and this oracle moves 10-13% in seconds."""
    trigger = 2.0 * REQUOTE_AT_RING_FRACTION
    drifted = RestingQuote(bid_price=ORACLE * (1 - (trigger + 0.01) / 100),
                           ask_price=0.1001)
    d = decide(view(), drifted)
    assert d.action is LoopAction.QUOTE
    assert "drifted" in d.reason


def test_drift_is_checked_on_both_sides():
    trigger = 2.0 * REQUOTE_AT_RING_FRACTION
    drifted = RestingQuote(bid_price=0.0999,
                           ask_price=ORACLE * (1 + (trigger + 0.01) / 100))
    assert decide(view(), drifted).action is LoopAction.QUOTE


def test_the_trigger_sits_inside_the_ring_not_on_it():
    assert 0.0 < REQUOTE_AT_RING_FRACTION < 1.0


def test_every_withdrawal_says_why():
    """A book that is down for a good reason and one that is down by accident
    look identical from outside."""
    for v in (view(trading_paused=True), view(session_ok=False),
              view(oracle=None), view(oracle_age_s=999)):
        d = decide(v, GOOD)
        assert d.action in (LoopAction.WITHDRAW, LoopAction.WAIT)
        assert d.reason and len(d.reason) > 10
