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
    commits the same coins twice.

    [review #22] The gate is CALLER-SUPPLIED now, not inferred from book
    shape: a routine TTL drain also leaves the book non-empty, and the
    inference one-shot-refused the startup arm with a false reason. The
    gathers add the gate exactly when a cancel is genuinely in flight."""
    allowed, reason = may_turn_on(
        _in(desired_on=False, book_is_empty=False,
            gates=["cancels_pending"]))
    assert not allowed
    assert reason == GATE_LABELS["cancels_pending"]

    # Book shape ALONE no longer refuses -- that is the TTL-drain case.
    allowed, _ = may_turn_on(_in(desired_on=False, book_is_empty=False))
    assert allowed


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

    # [v0.10.4 field report] SEAL THE IDENTITY. _gather_permuto and the
    # startup-state apply resolve _default_identity_factory(), which reads
    # the machine's REAL secrets.yaml -- and the moment the operator
    # actually registered, every "refuses until registered" assumption in
    # this file inverted, and the startup-on test would have called
    # _on_permuto_toggle(True) with a REGISTERED identity: a test suite one
    # start() away from placing live orders. Tests get an unregistered
    # fake, unconditionally.
    import gui.widgets.permuto as permuto_mod

    class _UnregisteredInfo:
        registered = False
        link_attempted = False
        backup_confirmed = False
        listing_verified = False
        user_id = None
        trading_address = None
        pubkey = "ab" * 48
        created_at = None

    class _FakeIdentity:
        @staticmethod
        def info():
            return _UnregisteredInfo()

    # [STARTINTENT] Seal the startup states too: load_startup_states reads
    # the machine's REAL QSettings, and a box with auto-arm stored would
    # seed dexie intent ON and invert this file's baseline chips.
    import gui.widgets.settings as settings_mod

    # [PERMUTO MASTER SWITCH] Seal this for the same reason. It reads the
    # machine's real QSettings, and an operator who switched Permuto off
    # would build a window with no _permuto_switch at all -- every chip
    # assertion in this file would fail on their box and nowhere else.
    real = permuto_mod._default_identity_factory
    real_loader = settings_mod.load_startup_states
    real_enabled = settings_mod.load_permuto_enabled
    permuto_mod._default_identity_factory = lambda: _FakeIdentity()
    settings_mod.load_startup_states = lambda: ("adopt", "off")
    settings_mod.load_permuto_enabled = lambda: True
    try:
        from gui.widgets.main_window import MainWindow

        yield MainWindow()
    finally:
        permuto_mod._default_identity_factory = real
        settings_mod.load_startup_states = real_loader
        settings_mod.load_permuto_enabled = real_enabled


def test_both_switches_start_honestly(window):
    """[INTENT v0.10.7] With no bridge the dexie book is UNKNOWN, and dexie
    offers rest on chain and survive this process -- so the chip must not
    claim flatness, and must not invent resting offers either. Permuto's
    unverified book is deliberately reported empty (arming is what
    reconciles it), so its chip says unverified rather than flat."""
    assert window._dexie_switch.status_text() == "stopped -- book unknown"
    assert window._permuto_switch.status_text()         == "stopped -- book unverified"


def test_dexie_click_records_intent_and_the_chip_names_the_gate(window):
    """[INTENT v0.10.7] Clicks are never refused: the slider records what
    the operator wants, and the chip explains why reality is not following
    yet -- gates-first, so "the engine is not running" is what shows."""
    seen = []
    window._dexie_switch.refused.connect(seen.append)
    window._dexie_switch.click()
    assert seen == [], "the intent slider must never refuse a click"
    assert window._dexie_desired_on is True
    assert window._dexie_switch.status_text() == "blocked"
    assert "engine is not running" in window._dexie_switch._chip.toolTip()


def test_permuto_refuses_until_registered(window):
    """The runner is wired now, so registration is the only remaining gate.

    `not_wired` existed while nothing owned a QuoteRunner and the switch could
    only ever have claimed ON over no session and no orders. PermutoLive owns
    one, so that gate is gone and the operator's own decision -- built, but
    disabled until registered -- is what holds.
    """
    seen = []
    window._permuto_switch.refused.connect(seen.append)
    window._permuto_switch.click()
    assert seen == [], "the intent slider must never refuse a click"
    assert window._permuto_desired_on is True
    assert window._permuto_runner is None, (
        "the START must be deferred while the gate holds -- constructing "
        "the runner here would authenticate against the real venue")
    assert window._permuto_switch.status_text() == "blocked"
    assert "not registered" in window._permuto_switch._chip.toolTip()


def test_permuto_reports_its_own_book_rather_than_assuming(window):
    """A stop in flight must read STOPPING, not OFF.

    The live session reports empty only once its cancel is acknowledged, so
    the switch cannot claim OFF over orders that may still be resting.
    """
    class _Live:
        @staticmethod
        def book_is_empty():
            return False

    window._permuto_runner = _Live()
    try:
        assert window._gather_permuto().book_is_empty is False
    finally:
        window._permuto_runner = None

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
        def has_data():
            return True

        @staticmethod
        def get_offers_summary():
            return {"pending": 3.0}

        @staticmethod
        def posting_gate_reasons():
            return set()

    class _Bridge:
        metrics_service = _Svc()

    monkeypatch.setattr(window, "_bridge", _Bridge())
    window._dexie_intent_synced = True      # do not adopt engine state here
    window._dexie_desired_on = False
    window._dexie_switch.refresh()
    assert "resting, draining" in window._dexie_switch.status_text()

    # And it reads flat only once the count actually reaches zero.
    _Svc.get_offers_summary = staticmethod(lambda: {"pending": 0.0})
    window._dexie_switch.refresh()
    assert window._dexie_switch.status_text() == "stopped -- flat"


def test_the_gui_pause_is_not_treated_as_a_protection_gate(window, monkeypatch):
    """`gui` IS this switch. Counting it would make the switch permanently
    unable to turn itself back on."""
    class _Svc:
        @staticmethod
        def has_data():
            return True

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


def test_a_disconnected_metrics_service_is_not_an_empty_book(window, monkeypatch):
    """[review] Every gauge defaults to 0.0 when nothing has been scraped, so
    pending==0 rendered as DEXIE OFF over a book that may still be takeable --
    contradicting the unknown-is-not-empty contract in the same method."""
    class _Svc:
        @staticmethod
        def has_data():
            return False

        @staticmethod
        def get_offers_summary():
            return {"pending": 0.0}       # the default, not an observation

        @staticmethod
        def posting_gate_reasons():
            return set()

    class _Bridge:
        metrics_service = _Svc()

    monkeypatch.setattr(window, "_bridge", _Bridge())
    assert window._dexie_book_is_empty() is False


def test_a_pause_button_pause_is_not_hidden_by_the_switch(window, monkeypatch):
    """`gui` is only OUR pause while the switch is OFF.

    With intent ON, a gui gate means the Pause/Resume button did it -- and
    dropping it unconditionally showed DEXIE ON over a paused Step 8.
    """
    class _Svc:
        @staticmethod
        def has_data():
            return True

        @staticmethod
        def get_offers_summary():
            return {"pending": 0.0}

        @staticmethod
        def posting_gate_reasons():
            return {"gui"}

    class _Bridge:
        metrics_service = _Svc()

    monkeypatch.setattr(window, "_bridge", _Bridge())
    window._bot_running = True
    window._dexie_intent_synced = True
    window._dexie_desired_on = True
    assert "gui" in window._gather_dexie().gates
    window._dexie_switch.refresh()
    assert window._dexie_switch.status_text() == "blocked"
    assert "Pause/Resume" in window._dexie_switch._chip.toolTip()
    window._bot_running = False


def test_the_switch_adopts_an_already_trading_engine(window, monkeypatch):
    """EngineBridge auto-starts the engine without creating pause.flag, so a
    fresh GUI attaches to one that is already trading -- and the intent
    defaulted to False, showing DEXIE OFF over a live book."""
    class _Svc:
        @staticmethod
        def has_data():
            return True

        @staticmethod
        def get_offers_summary():
            return {"pending": 0.0}

        @staticmethod
        def posting_gate_reasons():
            return set()          # ungated: it IS posting

        @staticmethod
        def posting_gates_published():
            # The gauges are genuinely present in this fake's world --
            # required since the sync learned to wait for evidence rather
            # than reading a pre-publication scrape as "trading".
            return True

    class _Bridge:
        metrics_service = _Svc()

    monkeypatch.setattr(window, "_bridge", _Bridge())
    window._dexie_intent_synced = False
    window._dexie_desired_on = False
    window._sync_dexie_intent_from_engine()
    assert window._dexie_desired_on is True


# --------------------------------------------------------------------------- #
# [review] The switch must not assert what it has not checked
# --------------------------------------------------------------------------- #

def test_a_blocked_tick_gates_the_switch(window):
    """`not_wired` was removed on the premise that owning a QuoteRunner
    guarantees a usable session. Nothing enforced that premise and it was
    false -- the live session was built with an empty token, every tick came
    back "blocked", and the toolbar painted PERMUTO ON over a loop that had
    never placed an order. The session bootstrap fixes the cause; this stops
    the switch claiming something it never checked."""
    window._permuto_last_blocked = True
    try:
        assert "blocked" in window._gather_permuto().gates
    finally:
        window._permuto_last_blocked = False
    assert "blocked" not in window._gather_permuto().gates


def test_the_permuto_book_is_unverified_until_something_looks(window):
    """Permuto orders rest at a REMOTE venue and survive this process, so a
    fresh GUI has genuinely not checked. It still reports empty -- refusing
    to arm would be worse, since arming is what reconciles -- but it must not
    claim verification it does not have."""
    assert window._gather_permuto().book_verified is False
    assert window._gather_permuto().book_is_empty is True, (
        "an unverified book must still be armable")


def test_a_tick_that_reached_the_venue_verifies_the_book(window):
    for action in ("quote", "hold", "withdraw", "wait"):
        window._permuto_book_confirmed_empty = False
        window._on_permuto_tick(
            type("R", (), {"action": action, "ok": True, "error": ""})())
        assert window._gather_permuto().book_verified, action


def test_a_blocked_tick_does_not_verify_the_book(window):
    """Blocked means we never reached the venue, so nothing was observed."""
    window._permuto_book_confirmed_empty = False
    window._on_permuto_tick(
        type("R", (), {"action": "blocked", "ok": False, "error": "x"})())
    try:
        assert not window._gather_permuto().book_verified
    finally:
        window._permuto_last_blocked = False


def test_any_non_trading_tick_gates_the_switch(window):
    """[review] Only the literal "blocked" used to gate it. withdraw is what a
    venue pause, a missing oracle or an unreadable account produce -- all
    states holding no quotes -- so those cleared the latch and the toolbar
    resolved to ON over a loop doing nothing."""
    for action in ("withdraw", "error", "wait", "blocked"):
        window._on_permuto_tick(
            type("R", (), {"action": action, "ok": True, "error": ""})())
        gates = window._gather_permuto().gates
        assert gates & {"blocked", "not_quoting"}, action


def test_a_trading_tick_clears_the_gate(window):
    for action in ("quote", "hold"):
        window._on_permuto_tick(
            type("R", (), {"action": "withdraw", "ok": True, "error": ""})())
        assert window._gather_permuto().gates & {"blocked", "not_quoting"}
        window._on_permuto_tick(
            type("R", (), {"action": action, "ok": True, "error": ""})())
        gates = window._gather_permuto().gates
        assert not (gates & {"blocked", "not_quoting"}), action


def test_stopping_clears_the_blocked_latch_so_the_switch_can_rearm(window):
    """[release review] The latch is written only by the tick handler, so a
    stop during a blocked spell -- the Sunday pause is the guaranteed one --
    froze it True with no ticks left to clear it. Every later arm attempt
    was refused with stale text until the GUI restarted: an off->on cycle
    during the pause locked the operator out of the contest open."""
    window._on_permuto_tick(
        type("R", (), {"action": "withdraw", "ok": True, "error": ""})())
    assert window._gather_permuto().gates & {"blocked", "not_quoting"}

    window._permuto_desired_on = True
    window._on_permuto_toggle(False)     # operator turns it OFF

    gates = window._gather_permuto().gates
    assert not (gates & {"blocked", "not_quoting"}), (
        "a stale latch from the stopped session still gates the next one")


def test_arming_does_not_paint_ON_before_the_first_pass(window, monkeypatch):
    """[review round 11] desired_on flips true at the click, and nothing had
    yet proven the loop can authenticate, read the account or place -- so the
    very next refresh painted PERMUTO ON over an unstarted session. The
    starting seed gates it until the first quote/hold tick clears it."""
    class _FakeLive:
        def __init__(self):
            from PySide6.QtCore import QObject, Signal

            class _Sig(QObject):
                s = Signal(object)
            self._t, self._s = _Sig(), _Sig()
            self.ticked, self.stopped = self._t.s, self._s.s

        def start(self):
            pass

        def book_is_empty(self):
            return False

    monkeypatch.setattr(window, "_make_permuto_live", lambda: _FakeLive())
    # This test is about the STARTING latch, not the registration gate:
    # let the deferred-start check pass so the fake actually arms.
    monkeypatch.setattr("gui.services.venue_control.may_turn_on",
                        lambda inputs: (True, ""))
    window._on_permuto_toggle(True)
    try:
        gates = window._gather_permuto().gates
        assert "starting" in gates, "armed with nothing proven reads as ON"
        # The first healthy tick clears it, like every later recovery.
        window._on_permuto_tick(
            type("R", (), {"action": "quote", "ok": True, "error": ""})())
        assert "starting" not in window._gather_permuto().gates
    finally:
        window._permuto_desired_on = False
        window._permuto_runner = None
        window._permuto_last_blocked = False
        window._permuto_last_action = ""


def test_the_intent_sync_waits_for_the_gate_family(window, monkeypatch):
    """[v0.10.1 field report] The metrics endpoint answers before the first
    cycle publishes the gate family, and reading that empty scrape as
    "ungated, therefore trading" adopted an ON intent nobody expressed --
    the operator opened the GUI to DEXIE BLOCKED over their own pause."""
    class _Svc:
        @staticmethod
        def posting_gate_reasons():
            return set()          # empty -- but only because nothing is
                                  # published yet

        @staticmethod
        def posting_gates_published():
            return False

    class _Bridge:
        metrics_service = _Svc()

    monkeypatch.setattr(window, "_bridge", _Bridge(), raising=False)
    window._dexie_intent_synced = False
    window._dexie_desired_on = False
    try:
        window._sync_dexie_intent_from_engine()
        assert not window._dexie_intent_synced, "synced from no evidence"
        assert not window._dexie_desired_on, (
            "an unpublished scrape was read as an already-trading engine")
    finally:
        window._dexie_intent_synced = False
        window._bridge = None


# --------------------------------------------------------------------------- #
# [startup state] Settings > Startup: requests, never overrides
# --------------------------------------------------------------------------- #

def test_permuto_startup_on_records_intent_and_defers_behind_the_gate(window):
    """[R2 #17] The startup wish is HELD, not dropped: with an
    unregistered identity the intent is recorded, the start deferred, and
    the chip names the gate -- exactly like a click. The setting still
    never starts a session through a closed gate."""
    window._startup_permuto = "on"
    window._startup_permuto_applied = False
    try:
        window._apply_permuto_startup_state()
        assert window._permuto_desired_on is True, "the wish was dropped"
        assert window._permuto_runner is None, (
            "a session must not start through a closed gate")
        assert window._permuto_switch.status_text() == "blocked"
        assert "not registered" in window._permuto_switch._chip.toolTip()
    finally:
        window._startup_permuto = "off"
        window._permuto_desired_on = False
        window._refresh_venue_switches()


def test_permuto_startup_state_applies_exactly_once(window):
    """The reboot story must not become a re-apply loop: the request is
    consumed on first application."""
    calls = []
    real = window._on_permuto_toggle
    window._on_permuto_toggle = lambda want: calls.append(want)
    window._startup_permuto = "on"
    window._startup_permuto_applied = False
    try:
        window._apply_permuto_startup_state()
        window._apply_permuto_startup_state()
        assert calls == [True]
    finally:
        window._on_permuto_toggle = real
        window._startup_permuto = "off"


def test_permuto_startup_off_arms_nothing(window):
    window._startup_permuto = "off"
    window._startup_permuto_applied = False
    window._apply_permuto_startup_state()
    assert not window._permuto_desired_on


def test_dexie_startup_request_outranks_adopt_but_not_the_gates(window,
                                                               monkeypatch):
    """With the engine visibly paused, 'dexie: on' resumes it through the
    same path as a click -- and with gates real, a refusal is loud."""
    calls = []

    class _Svc:
        @staticmethod
        def posting_gates_published():
            return True

        @staticmethod
        def posting_gate_reasons():
            return {"gui"}          # paused by the operator's own flag

        @staticmethod
        def has_data():
            return True

        @staticmethod
        def get_offers_summary():
            return {"pending": 0.0}

    class _Bridge:
        metrics_service = _Svc()

        def start_engine(self):
            calls.append("start")

        def resume_trading(self):
            calls.append("resume")

    monkeypatch.setattr(window, "_bridge", _Bridge(), raising=False)
    monkeypatch.setattr(window, "_bot_running", True, raising=False)
    window._dexie_intent_synced = False
    window._dexie_desired_on = False
    window._startup_dexie = "on"
    try:
        window._sync_dexie_intent_from_engine()
        # Adopt would have said OFF (a gate holds); the explicit request
        # resumes instead -- through _on_dexie_toggle, hence resume_trading.
        assert "resume" in calls, "the startup request was not applied"
        assert window._dexie_desired_on
    finally:
        window._startup_dexie = "adopt"
        window._dexie_intent_synced = False
        window._dexie_desired_on = False
        window._bridge = None


def test_startup_states_loader_defaults_are_safe(monkeypatch):
    """Unset or garbage QSettings values must land on adopt/off -- the
    states that arm nothing by themselves."""
    from gui.widgets.settings import load_startup_states

    from PySide6.QtCore import QSettings
    settings = QSettings("XOP", "XOPTrader")
    settings.beginGroup("startup")
    old_d, old_p = settings.value("dexie"), settings.value("permuto")
    settings.setValue("dexie", "sideways")
    settings.setValue("permuto", 42)
    settings.endGroup()
    try:
        assert load_startup_states() == ("adopt", "off")
    finally:
        settings.beginGroup("startup")
        if old_d is None:
            settings.remove("dexie")
        else:
            settings.setValue("dexie", old_d)
        if old_p is None:
            settings.remove("permuto")
        else:
            settings.setValue("permuto", old_p)
        settings.endGroup()


# --------------------------------------------------------------------------- #
# [review #26] The cancel-all latch, gate, and deferred-resume convergence
# --------------------------------------------------------------------------- #

class _RecordingBridge:
    """Minimal bridge fake that records the calls the latch logic makes."""

    def __init__(self):
        self.calls = []
        self._pending_marker = False

    def pause_trading(self):
        self.calls.append("pause")

    def resume_trading(self):
        self.calls.append("resume")

    def cancel_all_offers(self):
        self.calls.append("cancel_all")
        return True

    def mark_cancel_all_pending(self):
        self._pending_marker = True

    def clear_cancel_all_pending(self):
        self._pending_marker = False

    def cancel_all_pending(self):
        return self._pending_marker


def test_off_is_pause_only(window, monkeypatch):
    b = _RecordingBridge()
    monkeypatch.setattr(window, "_bridge", b)
    window._on_dexie_toggle(False)
    assert "pause" in b.calls
    assert "cancel_all" not in b.calls, (
        "[STOPDRAIN] the operator's routine OFF must not force-cancel -- "
        "the TTL drain owns the book now")


def test_cancel_all_sets_and_persists_the_latch(window, monkeypatch):
    b = _RecordingBridge()
    monkeypatch.setattr(window, "_bridge", b)
    window._on_dexie_cancel_all()
    assert "cancel_all" in b.calls
    assert window._dexie_cancel_all_pending is True
    assert b.cancel_all_pending() is True, (
        "[review #8] the latch must survive a GUI restart via the marker")


def test_on_over_unconfirmed_cancel_defers_the_resume(window, monkeypatch):
    b = _RecordingBridge()
    monkeypatch.setattr(window, "_bridge", b)
    window._dexie_cancel_all_pending = True
    monkeypatch.setattr(window, "_dexie_book_is_empty", lambda: False)
    window._bot_running = True
    window._on_dexie_toggle(True)
    assert window._dexie_desired_on is True, "intent is the operator's"
    assert "resume" not in b.calls, (
        "[review #18-shape] resuming would post over unconfirmed cancels")
    assert window._dexie_resume_deferred is True
    window._bot_running = False


def test_convergence_applies_exactly_one_deferred_resume(window, monkeypatch):
    b = _RecordingBridge()
    monkeypatch.setattr(window, "_bridge", b)
    window._dexie_desired_on = True
    window._dexie_resume_deferred = True
    window._dexie_cancel_all_pending = False
    window._bot_running = True
    window._bot_paused = False
    window._refresh_venue_switches()
    window._refresh_venue_switches()
    assert b.calls.count("resume") == 1, (
        "the deferred resume is exactly-once, not per-tick")
    window._bot_running = False


def test_pause_button_resume_honours_the_latch(window, monkeypatch):
    b = _RecordingBridge()
    monkeypatch.setattr(window, "_bridge", b)
    window._bot_paused = True
    window._dexie_cancel_all_pending = True
    monkeypatch.setattr(window, "_dexie_book_is_empty", lambda: False)
    window._on_pause_resume()
    assert "resume" not in b.calls, (
        "[review #18] the Resume button must not bypass the latch the "
        "switch honours")
    assert window._dexie_resume_deferred is True


def test_cancel_all_with_no_bridge_refuses_and_leaves_the_latch(window):
    seen = []
    window._on_switch_refused = lambda r: seen.append(r)
    window._bridge = None
    window._dexie_cancel_all_pending = False
    window._on_dexie_cancel_all()
    assert seen, "a disconnected bridge must refuse loudly"
    assert window._dexie_cancel_all_pending is False, (
        "no latch without a delivered request")


def test_cancels_pending_gates_the_gather_and_retires_on_flat(
        window, monkeypatch):
    b = _RecordingBridge()
    monkeypatch.setattr(window, "_bridge", b)
    window._dexie_cancel_all_pending = True
    monkeypatch.setattr(window, "_dexie_book_is_empty", lambda: False)
    assert "cancels_pending" in window._gather_dexie().gates

    monkeypatch.setattr(window, "_dexie_book_is_empty", lambda: True)
    window._gather_dexie()
    assert window._dexie_cancel_all_pending is False, (
        "a verifiably flat book retires the latch")
    assert b.cancel_all_pending() is False, "and its persisted marker"


def test_convergence_needs_the_deferred_flag_not_just_a_gui_gate(
        window, monkeypatch):
    b = _RecordingBridge()
    monkeypatch.setattr(window, "_bridge", b)
    window._dexie_desired_on = True
    window._dexie_resume_deferred = False
    window._bot_running = True
    window._bot_paused = False
    window._refresh_venue_switches()
    assert "resume" not in b.calls, (
        "[review #16] a bare gui gate must not auto-resume -- that would "
        "override the Pause button")
    window._bot_running = False
    window._dexie_desired_on = False


def test_deferred_permuto_on_shows_not_running_when_the_gate_clears(
        window, monkeypatch):
    """[R2 #17/#25] Intent ON, no runner, registration gate cleared: the
    chip must say the loop is not running with re-flip guidance -- never
    green 'quoting' over nothing."""
    from gui.services.venue_control import SwitchInputs

    window._permuto_desired_on = True
    assert window._permuto_runner is None
    # The switch captured its gather at construction -- patch that
    # reference, simulating "registration gate cleared, still no runner".
    monkeypatch.setattr(
        window._permuto_switch, "_gather",
        lambda: SwitchInputs(desired_on=True,
                             gates=frozenset({"not_running"})))
    window._permuto_switch.refresh()
    assert window._permuto_switch.status_text() == "blocked"
    assert "flip the switch OFF" in window._permuto_switch._chip.toolTip()
    window._permuto_desired_on = False


# --------------------------------------------------------------------------- #
# [STARTINTENT v0.10.11] A stored "start with dexie on" is the operator's
# intent: the slider paints ON immediately and the chip carries the boot
# story ("starting") instead of the false "the engine is not running".
# --------------------------------------------------------------------------- #


def _sealed_window(startup_dexie):
    """A fresh MainWindow with the identity sealed and startup states forced."""
    from PySide6.QtWidgets import QApplication

    QApplication.instance() or QApplication([])

    import gui.widgets.permuto as permuto_mod
    import gui.widgets.settings as settings_mod

    class _UnregisteredInfo:
        registered = False
        link_attempted = False
        backup_confirmed = False
        listing_verified = False
        user_id = None
        trading_address = None
        pubkey = "ab" * 48
        created_at = None

    class _FakeIdentity:
        @staticmethod
        def info():
            return _UnregisteredInfo()

    real_identity = permuto_mod._default_identity_factory
    real_loader = settings_mod.load_startup_states
    real_enabled = settings_mod.load_permuto_enabled
    permuto_mod._default_identity_factory = lambda: _FakeIdentity()
    settings_mod.load_startup_states = lambda: (startup_dexie, "off")
    settings_mod.load_permuto_enabled = lambda: True
    try:
        from gui.widgets.main_window import MainWindow

        return MainWindow()
    finally:
        permuto_mod._default_identity_factory = real_identity
        settings_mod.load_startup_states = real_loader
        settings_mod.load_permuto_enabled = real_enabled


def test_a_stored_on_request_paints_intent_before_the_engine():
    w = _sealed_window("on")
    assert w._dexie_desired_on is True
    w._dexie_switch.refresh()
    assert w._dexie_switch.status_text() == "starting"
    assert w._dexie_switch._toggle.isChecked()


def test_the_boot_window_expires_into_the_real_gate():
    """An engine that never comes up must stop claiming boot progress."""
    w = _sealed_window("on")
    w._gui_started_at -= 1000.0
    w._dexie_switch.refresh()
    assert w._dexie_switch.status_text() == "blocked"


def test_an_analyzing_engine_reads_starting_not_engine_down():
    """"Analyzing" clears _bot_running, but the process is up and working --
    any ON intent deserves the boot story, not "engine is not running"."""
    w = _sealed_window("adopt")
    w._dexie_desired_on = True
    w._on_bot_status_changed("Analyzing")
    w._dexie_switch.refresh()
    assert w._dexie_switch.status_text() == "starting"


def test_an_unpublished_gate_family_reads_starting_not_quoting():
    """The metrics endpoint comes up before the first cycle publishes the
    gate family; an empty scrape is evidence of starting, not of trading."""
    w = _sealed_window("on")

    class _Svc:
        @staticmethod
        def has_data():
            return False

        @staticmethod
        def posting_gates_published():
            return False

        @staticmethod
        def posting_gate_reasons():
            return set()

        @staticmethod
        def offers_published():
            return False

        @staticmethod
        def get_offers_summary():
            return {}

    class _Bridge:
        metrics_service = _Svc()

    w._bridge = _Bridge()
    w._bot_running = True
    w._dexie_switch.refresh()
    assert w._dexie_switch.status_text() == "starting"


def test_a_failed_launch_retires_the_starting_story(monkeypatch):
    from gui.widgets import main_window as mw_mod

    w = _sealed_window("on")
    monkeypatch.setattr(mw_mod.QMessageBox, "critical",
                        staticmethod(lambda *a, **k: None))
    w._on_engine_start_failed("boom")
    assert w._startup_dexie == ""
    w._dexie_switch.refresh()
    assert w._dexie_switch.status_text() == "blocked"


def test_a_presync_click_outranks_the_stored_request():
    """The operator's explicit toggle wins over the stored startup request:
    the gate-publish application must find nothing left to apply."""
    w = _sealed_window("on")
    w._on_dexie_toggle(False)
    assert w._startup_dexie == ""
    assert w._dexie_desired_on is False
