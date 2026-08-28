"""The two-venue trading switch.

Three rules carry it, and each is a bug that has already happened somewhere in
this project: off is always allowed, on is refused while a protection latch
holds, and OFF means the book is gone rather than that posting stopped.
"""

from __future__ import annotations

import pytest

from gui.services.venue_control import (
    GATE_LABELS,
    SwitchInputs,
    VenueState,
    may_turn_off,
    may_turn_on,
    resolve_state,
)


def _in(**kw) -> SwitchInputs:
    kw.setdefault("book_is_empty", True)
    if "gates" in kw:
        kw["gates"] = frozenset(kw["gates"])
    return SwitchInputs(**kw)


# --------------------------------------------------------------------------- #
# Rule 1: nothing may stand between an operator and stopping
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("gates", [
    [], ["breaker"], ["watchdog"], ["wallet_circuit", "flash_crash"],
    ["something_invented_later"],
])
def test_off_is_always_allowed(gates):
    allowed, reason = may_turn_off(_in(desired_on=True, gates=gates))
    assert allowed
    assert reason == ""


def test_off_is_allowed_even_mid_stop(_=None):
    allowed, _reason = may_turn_off(
        _in(desired_on=False, book_is_empty=False))
    assert allowed


# --------------------------------------------------------------------------- #
# Rule 2: on is refused while a latch holds, and says which
# --------------------------------------------------------------------------- #

def test_on_is_allowed_when_nothing_objects():
    allowed, reason = may_turn_on(_in(desired_on=False))
    assert allowed and reason == ""


@pytest.mark.parametrize("gate", sorted(GATE_LABELS))
def test_every_known_gate_refuses_and_explains(gate):
    allowed, reason = may_turn_on(_in(desired_on=False, gates=[gate]))
    assert not allowed
    assert reason == GATE_LABELS[gate]
    assert reason, "a refusal with no reason is a flaky-looking switch"


def test_an_unrecognised_gate_still_refuses():
    """Failing open on an unknown reason would make every future gate a
    silent no-op here."""
    allowed, reason = may_turn_on(_in(desired_on=False, gates=["from_2027"]))
    assert not allowed
    assert reason == "from_2027"


def test_the_most_serious_gate_is_the_one_named():
    """An operator acts on one thing; a switch reciting four reasons teaches
    people to stop reading it."""
    allowed, reason = may_turn_on(
        _in(desired_on=False, gates=["not_registered", "watchdog", "breaker"]))
    assert not allowed
    assert reason == GATE_LABELS["watchdog"]


def test_turning_on_over_an_unconfirmed_stop_is_refused():
    """Posting a fresh book on top of cancel spends that have not settled
    commits the same coins twice."""
    allowed, reason = may_turn_on(
        _in(desired_on=False, book_is_empty=False))
    assert not allowed
    assert reason == GATE_LABELS["cancels_pending"]


# --------------------------------------------------------------------------- #
# Rule 3: OFF means the book is gone
# --------------------------------------------------------------------------- #

def test_off_with_a_live_book_reports_stopping_not_off():
    """A secure cancel spends coins on chain, so it settles when those spends
    confirm and not when the RPC returns. Six four-hour-old bids were picked
    off on 2026-08-25 in exactly this window."""
    assert resolve_state(
        _in(desired_on=False, book_is_empty=False)) is VenueState.STOPPING


def test_off_with_an_empty_book_is_off():
    assert resolve_state(_in(desired_on=False)) is VenueState.OFF


def test_intent_alone_cannot_retire_a_takeable_offer():
    # The operator asked to stop; the book says otherwise, and the book wins.
    state = resolve_state(_in(desired_on=False, book_is_empty=False))
    assert state is not VenueState.OFF


# --------------------------------------------------------------------------- #
# The state machine is total
# --------------------------------------------------------------------------- #

def test_on_requested_and_permitted_is_on():
    assert resolve_state(_in(desired_on=True)) is VenueState.ON


def test_on_requested_and_gated_is_blocked_never_on():
    assert resolve_state(
        _in(desired_on=True, gates=["breaker"])) is VenueState.BLOCKED


def test_a_gate_appearing_under_a_running_venue_blocks_it():
    """The breaker trips while trading. The switch must stop claiming ON."""
    running = _in(desired_on=True)
    assert resolve_state(running) is VenueState.ON
    tripped = _in(desired_on=True, gates=["breaker"])
    assert resolve_state(tripped) is VenueState.BLOCKED


@pytest.mark.parametrize("desired_on", [True, False])
@pytest.mark.parametrize("book_is_empty", [True, False])
@pytest.mark.parametrize("gates", [[], ["breaker"], ["engine_down"]])
def test_every_input_maps_to_exactly_one_state(desired_on, book_is_empty,
                                               gates):
    state = resolve_state(
        _in(desired_on=desired_on, book_is_empty=book_is_empty, gates=gates))
    assert isinstance(state, VenueState)


def test_the_two_venues_are_independent():
    """Both on, either alone, or neither -- and one venue's gate must not
    reach the other."""
    dexie = _in(desired_on=True, gates=["breaker"])
    permuto = _in(desired_on=True)
    assert resolve_state(dexie) is VenueState.BLOCKED
    assert resolve_state(permuto) is VenueState.ON


# --------------------------------------------------------------------------- #
# Main-window wiring
#
# The state machine above is pure; these check that the window feeds it the
# truth, especially when it cannot read something.
# --------------------------------------------------------------------------- #

pytest.importorskip("PySide6")


@pytest.fixture(scope="module")
def window():
    from PySide6.QtWidgets import QApplication

    QApplication.instance() or QApplication([])
    from gui.widgets.main_window import MainWindow

    return MainWindow()


def test_both_switches_start_off(window):
    assert window._dexie_switch.text() == "DEXIE OFF"
    assert window._permuto_switch.text() == "PERMUTO OFF"


def test_dexie_refuses_while_the_engine_is_down(window):
    seen = []
    window._dexie_switch.refused.connect(seen.append)
    window._dexie_switch.click()
    assert seen == ["the engine is not running"]
    assert window._dexie_switch.text() == "DEXIE OFF"


def test_permuto_refuses_until_registered(window):
    seen = []
    window._permuto_switch.refused.connect(seen.append)
    window._permuto_switch.click()
    assert seen == ["this identity is not registered with the venue"]


def test_an_unreadable_gate_state_fails_closed(window, monkeypatch):
    """Guessing "no gate" is how a switch turns trading on over a tripped
    breaker."""
    window._bot_running = True

    class _Boom:
        @property
        def metrics_service(self):
            raise RuntimeError("metrics gone")

    monkeypatch.setattr(window, "_bridge", _Boom())
    gathered = window._gather_dexie()
    assert "breaker" in gathered.gates
    window._bot_running = False


def test_an_unreadable_book_reports_not_empty(window, monkeypatch):
    """A secure cancel settles on chain. While the book cannot be read, the
    honest reading is that an offer may still be takeable."""
    class _Boom:
        @property
        def metrics_service(self):
            raise RuntimeError("metrics gone")

    monkeypatch.setattr(window, "_bridge", _Boom())
    assert window._dexie_book_is_empty() is False


def test_a_resting_book_shows_stopping_rather_than_off(window, monkeypatch):
    class _Svc:
        @staticmethod
        def get_offers_summary():
            return {"pending": 3.0}

        @staticmethod
        def posting_gate_reasons():
            return set()

    class _Bridge:
        metrics_service = _Svc()

    monkeypatch.setattr(window, "_bridge", _Bridge())
    window._dexie_desired_on = False
    window._dexie_switch.refresh()
    assert window._dexie_switch.text() == "DEXIE STOPPING"

    # And it becomes OFF only once the count actually reaches zero.
    _Svc.get_offers_summary = staticmethod(lambda: {"pending": 0.0})
    window._dexie_switch.refresh()
    assert window._dexie_switch.text() == "DEXIE OFF"


def test_the_gui_pause_is_not_treated_as_a_protection_gate(window, monkeypatch):
    """`gui` IS this switch. Counting it would make the switch permanently
    unable to turn itself back on."""
    class _Svc:
        @staticmethod
        def get_offers_summary():
            return {"pending": 0.0}

        @staticmethod
        def posting_gate_reasons():
            return {"gui", "dry_run"}

    class _Bridge:
        metrics_service = _Svc()

    monkeypatch.setattr(window, "_bridge", _Bridge())
    window._bot_running = True
    assert window._gather_dexie().gates == frozenset()
    window._bot_running = False
