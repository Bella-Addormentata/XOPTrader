"""The Settings > Advanced master switch for the Permuto market maker.

The switch promises something stronger than "Permuto is not quoting", which
the toolbar switch already said. It promises Permuto is not THERE: no
toolbar control, no sidebar entry, no page -- and, the part an operator
cannot see and therefore the part worth testing, no identity read and no
venue traffic.

That last clause is the one that decays silently. Two paths read
``secrets.yaml`` on a timer whether or not anybody ever arms Permuto:
``VenueSwitch.__init__`` calls ``refresh()`` which calls ``_gather_permuto``,
and ``_refresh_venue_switches`` calls it again on every bridge tick. Hiding a
widget does not stop either. So the tests below assert on the FACTORY -- if
it is never called, nothing was read -- rather than on what is visible.

The index tests exist because of a specific incident: ``_PAGE_SETTINGS``
stayed 9 when Permuto took index 9, and the first-run "you have no config"
redirect opened a key-generation page instead of Settings. Any design that
gates by shortening ``_NAV_ITEMS`` reintroduces it, so the disabled window is
asserted to have exactly as many pages as the enabled one.
"""

from __future__ import annotations

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication, QWidget  # noqa: E402


#: The QSettings keys this module writes. Snapshotted and restored around
#: every test by the autouse fixture below.
_TOUCHED_KEYS = (
    ("permuto", "enabled"),
    ("permuto", "curfew_enabled"),
    ("startup", "permuto"),
    ("startup", "dexie"),
)


@pytest.fixture(autouse=True)
def preserve_operator_settings():
    """Put the operator's store back, whatever the test did to it.

    This module's whole subject is code that WRITES startup preferences, and
    it runs against the real store because that is what the loaders read.
    QSettings.setPath would be tidier isolation but it is process-global:
    one call silently redirects every later test module in the same pytest
    process, including the ones asserting on real appearance and startup
    values. So snapshot, run, restore -- the same shape as
    test_startup_states_loader_defaults_are_safe.
    """
    from PySide6.QtCore import QSettings

    settings = QSettings("XOP", "XOPTrader")
    settings.sync()
    saved = {}
    for group, key in _TOUCHED_KEYS:
        settings.beginGroup(group)
        saved[(group, key)] = settings.value(key)
        settings.endGroup()
    try:
        yield
    finally:
        restore = QSettings("XOP", "XOPTrader")
        for (group, key), value in saved.items():
            restore.beginGroup(group)
            if value is None:
                restore.remove(key)
            else:
                restore.setValue(key, value)
            restore.endGroup()
        restore.sync()


@pytest.fixture(scope="module")
def app():
    instance = QApplication.instance() or QApplication(sys.argv)
    instance.setStyle("Fusion")
    yield instance


class _UnregisteredInfo:
    registered = False
    link_attempted = False
    backup_confirmed = False
    listing_verified = False
    user_id = None
    trading_address = None
    pubkey = "ab" * 48
    created_at = None


def _window(monkeypatch, *, enabled: bool):
    """A MainWindow with the master switch forced and the identity sealed.

    The identity factory counts its calls instead of merely being replaced:
    "the subsystem is off" is a claim about work NOT done, and a fake that
    silently answers is indistinguishable from a real one that reads the
    file. The counter is what tells them apart.
    """
    import gui.widgets.permuto as permuto_mod
    import gui.widgets.settings as settings_mod

    reads: list[int] = []

    class _FakeIdentity:
        @staticmethod
        def info():
            reads.append(1)
            return _UnregisteredInfo()

    monkeypatch.setattr(permuto_mod, "_default_identity_factory",
                        lambda: _FakeIdentity())
    monkeypatch.setattr(settings_mod, "load_startup_states",
                        lambda: ("adopt", "off"))
    monkeypatch.setattr(settings_mod, "load_permuto_enabled",
                        lambda: enabled)

    from gui.widgets.main_window import MainWindow

    return MainWindow(), reads


# --------------------------------------------------------------------------- #
# The loader
# --------------------------------------------------------------------------- #

def test_the_loader_defaults_to_disabled(monkeypatch):
    """Opt in, matching load_startup_states' own "off" for Permuto.

    A machine that has expressed no preference must not read a BLS key off
    disk on every tick for a venue nobody asked for. Not a claim that the
    venue is finished -- nothing is lost by the default, because the
    operator's startup and curfew preferences are kept and apply again the
    moment they switch it on.
    """
    from PySide6.QtCore import QSettings

    from gui.widgets.settings import load_permuto_enabled

    settings = QSettings("XOP", "XOPTrader")
    settings.beginGroup("permuto")
    previous = settings.value("enabled")
    settings.remove("enabled")
    settings.endGroup()
    try:
        assert load_permuto_enabled() is False
    finally:
        settings.beginGroup("permuto")
        if previous is None:
            settings.remove("enabled")
        else:
            settings.setValue("enabled", previous)
        settings.endGroup()


@pytest.mark.parametrize(
    "stored, expected",
    [
        (False, False), ("false", False), ("0", False), ("off", False),
        ("no", False), (True, True), ("true", True), ("1", True),
        ("on", True), ("yes", True),
        # Garbage reads as OFF, matching the default. Unparseable must fall
        # to the same side as unset, or the fail-safe direction depends on
        # which kind of corruption happened.
        ("sideways", False), ("", False),
    ],
)
def test_the_loader_reads_every_shape_qsettings_stores(stored, expected):
    """QSettings hands back a str on some platforms and a bool on others."""
    from PySide6.QtCore import QSettings

    from gui.widgets.settings import load_permuto_enabled

    settings = QSettings("XOP", "XOPTrader")
    settings.beginGroup("permuto")
    previous = settings.value("enabled")
    settings.setValue("enabled", stored)
    settings.endGroup()
    try:
        assert load_permuto_enabled() is expected
    finally:
        settings.beginGroup("permuto")
        if previous is None:
            settings.remove("enabled")
        else:
            settings.setValue("enabled", previous)
        settings.endGroup()


# --------------------------------------------------------------------------- #
# Disabled: the surfaces are gone
# --------------------------------------------------------------------------- #

def test_disabled_builds_no_toolbar_switch(app, monkeypatch):
    """Not built, not merely hidden.

    VenueSwitch.__init__ ends in refresh() -> _gather_permuto -> a read of
    secrets.yaml. Constructing it and hiding it would perform the very thing
    the switch says is not happening.
    """
    window, reads = _window(monkeypatch, enabled=False)
    try:
        assert window._permuto_switch is None
        assert window._permuto_switch_action is None
        assert reads == [], "the identity was read for a disabled subsystem"
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_disabled_hides_the_sidebar_entry_without_renumbering(app, monkeypatch):
    """The nav entry goes; the INDEX stays.

    _PAGE_SETTINGS is 10 because Permuto is 9. Shortening _NAV_ITEMS is the
    one implementation that satisfies "hide the tab" and breaks everything
    downstream of it.
    """
    from gui.widgets import main_window as mw
    from gui.widgets.sidebar import _NAV_ITEMS

    window, _reads = _window(monkeypatch, enabled=False)
    try:
        assert not window._sidebar.is_page_visible(mw._PAGE_PERMUTO)
        assert window._sidebar.is_page_visible(mw._PAGE_SETTINGS)
        assert len(_NAV_ITEMS) == 11
        assert _NAV_ITEMS[mw._PAGE_PERMUTO][0] == "Permuto"
        assert _NAV_ITEMS[mw._PAGE_SETTINGS][0] == "Settings"
        assert window._stacked.count() == 11
        assert window._stacked.widget(mw._PAGE_SETTINGS) is (
            window._settings_widget)
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_disabled_parks_a_placeholder_rather_than_the_real_page(
        app, monkeypatch):
    """Index 9 stays occupied, but PermutoWidget is never constructed.

    Its __init__ calls refresh(), which reads the identity -- so an
    instantiated-but-hidden page is another silent secrets read.
    """
    from gui.widgets import main_window as mw
    from gui.widgets.permuto import PermutoWidget

    window, reads = _window(monkeypatch, enabled=False)
    try:
        page = window._stacked.widget(mw._PAGE_PERMUTO)
        assert isinstance(page, QWidget)
        assert not isinstance(page, PermutoWidget)
        assert reads == []
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_a_hidden_nav_entry_is_not_reachable(app, monkeypatch):
    """Hidden must also mean unselectable.

    select_page() only bounds-checked its argument, so a hidden button was
    still programmatically selectable -- which would show the Permuto page
    with no highlighted entry anywhere in the rail.
    """
    from gui.widgets import main_window as mw

    window, _reads = _window(monkeypatch, enabled=False)
    try:
        # Each guard is exercised on its OWN path. Going only through
        # select_page would leave the guard in _on_button_clicked untested:
        # select_page refuses first, so the second guard could be deleted
        # with every assertion still green.
        window._sidebar.select_page(mw._PAGE_PERMUTO)
        assert window._sidebar._current_index != mw._PAGE_PERMUTO

        # _on_button_clicked is the SOLE emitter of page_changed, so it
        # guards independently of its callers.
        emitted: list[int] = []
        window._sidebar.page_changed.connect(emitted.append)
        try:
            window._sidebar._on_button_clicked(mw._PAGE_PERMUTO)
        finally:
            window._sidebar.page_changed.disconnect(emitted.append)
        assert emitted == [], "a hidden page was published to the stack"
        assert window._sidebar._current_index != mw._PAGE_PERMUTO

        window._switch_page(mw._PAGE_PERMUTO)
        assert window._stacked.currentIndex() == mw._PAGE_DASHBOARD
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


# --------------------------------------------------------------------------- #
# Disabled: nothing runs
# --------------------------------------------------------------------------- #

def test_disabled_gathers_without_touching_the_identity(app, monkeypatch):
    """The tick path, which is where the cost actually lives.

    _refresh_venue_switches runs on every bridge tick and _gather_permuto
    opens and parses secrets.yaml each time. The early return has to sit
    ABOVE that read -- and above the try, whose bare except maps any failure
    to "not_registered" and would report a registration problem for a
    subsystem the operator switched off.
    """
    window, reads = _window(monkeypatch, enabled=False)
    try:
        inputs = window._gather_permuto()
        assert "disabled" in inputs.gates
        assert "not_registered" not in inputs.gates
        assert inputs.desired_on is False
        assert inputs.book_is_empty is True

        for _ in range(20):
            window._refresh_venue_switches()
        assert reads == [], (
            "twenty ticks read the identity for a disabled subsystem")
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_the_disabled_gate_reads_as_english(app):
    """A raw token in the chip is a gate nobody registered."""
    from gui.services.venue_control import (
        GATE_LABELS,
        SwitchInputs,
        may_turn_on,
    )

    allowed, reason = may_turn_on(
        SwitchInputs(desired_on=True, gates=frozenset({"disabled"})))
    assert allowed is False
    assert reason == GATE_LABELS["disabled"]
    assert "Settings > Advanced" in reason


def test_the_disabled_gate_outranks_every_other_reason(app):
    """It answers a different question than the protection latches do.

    They say why a venue will not trade. This says why the venue is not
    here. An operator who switched Permuto off must not be told the dead
    man's switch fired.
    """
    from gui.services.venue_control import SwitchInputs, may_turn_on

    _allowed, reason = may_turn_on(SwitchInputs(
        desired_on=True,
        gates=frozenset({"disabled", "watchdog", "breaker", "engine_down"}),
    ))
    assert "switched off" in reason


def test_disabled_refuses_to_arm_but_never_refuses_to_stop(app, monkeypatch):
    """venue_control's first rule survives the master switch.

    "Nothing may stand between an operator and stopping" -- and the disable
    path itself goes through the OFF branch, so gating both directions would
    make the subsystem impossible to switch off safely.
    """
    window, _reads = _window(monkeypatch, enabled=False)
    refusals: list[str] = []
    window._on_switch_refused = refusals.append
    try:
        window._on_permuto_toggle(True)
        assert window._permuto_runner is None
        assert window._permuto_desired_on is False
        assert refusals and "switched off" in refusals[0]

        # The OFF branch must still run to completion.
        refusals.clear()
        window._on_permuto_toggle(False)
        assert refusals == []
        assert window._permuto_desired_on is False
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_disabled_ignores_a_stored_startup_arm(app, monkeypatch):
    """"Permuto: On at startup" must not outrank the master switch.

    Two independent guards, because the arm is a QTimer scheduled from
    set_bridge: the schedule is skipped, and the method refuses even if a
    timer from before the flip still fires.
    """
    window, _reads = _window(monkeypatch, enabled=False)
    toggles: list[bool] = []
    window._on_permuto_toggle = toggles.append
    try:
        assert window._startup_permuto == "off"
        window._startup_permuto = "on"       # as if the store said so
        window._apply_permuto_startup_state()
        assert toggles == [], "a disabled subsystem armed itself at startup"
        assert window._startup_permuto_applied is False, (
            "the guard must sit above the one-shot latch")
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_a_placeholder_page_does_not_break_the_session_builder(
        app, monkeypatch):
    """Pre-existing: _make_permuto_live read the page's attributes directly.

    Index 9 is not always a PermutoWidget -- the master switch parks a
    placeholder there, and _create_page_widget already substitutes one when
    a page's import or constructor fails. The direct reads turned that into
    "could not start Permuto quoting: '_placeholder' object has no attribute
    '_target_depth_usd'", which is not something an operator can act on.
    """
    import gui.services.permuto.live as live_mod

    window, _reads = _window(monkeypatch, enabled=False)
    seen: dict = {}

    class _FakeLive:
        def __init__(self, identity, **kwargs):
            seen.update(kwargs)

    monkeypatch.setattr(live_mod, "PermutoLive", _FakeLive)
    try:
        window._make_permuto_live()
        # Absent means "use PermutoLive's own defaults", so the sizing keys
        # must not be forwarded as None.
        assert "target_depth_usd" not in seen
        assert "max_position_usd" not in seen
        assert "curfew_enabled" in seen
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


# --------------------------------------------------------------------------- #
# Enabled: nothing was over-gated
# --------------------------------------------------------------------------- #

def test_enabled_still_builds_the_whole_subsystem(app, monkeypatch):
    """The other half of the switch.

    Every assertion above passes trivially against a build that removed
    Permuto outright. This is the one that says the gate is a gate.
    """
    from gui.widgets import main_window as mw
    from gui.widgets.permuto import PermutoWidget

    window, reads = _window(monkeypatch, enabled=True)
    try:
        assert window._permuto_switch is not None
        assert window._permuto_switch_action is not None
        # The baseline the hide assertions are measured against.
        assert window._permuto_switch_action.isVisible()
        assert window._sidebar.is_page_visible(mw._PAGE_PERMUTO)
        assert isinstance(window._stacked.widget(mw._PAGE_PERMUTO),
                          PermutoWidget)
        assert window._stacked.count() == 11
        assert reads, "the enabled subsystem never read its identity"

        inputs = window._gather_permuto()
        assert "disabled" not in inputs.gates
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


# --------------------------------------------------------------------------- #
# Master off means every other Permuto switch reads off
# --------------------------------------------------------------------------- #

def _settings_page(monkeypatch, *, enabled: bool):
    """A SettingsWidget with the master switch forced."""
    import gui.widgets.settings as settings_mod

    monkeypatch.setattr(settings_mod, "load_permuto_enabled",
                        lambda: enabled)
    return settings_mod.SettingsWidget()


def test_master_off_forces_permuto_at_startup_to_off(app, monkeypatch):
    """The invariant, applied on BUILD and not only on a click.

    A store carrying an older "Permuto: On at startup" -- which is exactly
    what a machine that ran the contest has -- would otherwise sit there
    reading On under a subsystem that cannot arm, and spring the moment the
    subsystem came back.
    """
    from PySide6.QtCore import QSettings

    page = _settings_page(monkeypatch, enabled=True)
    page._startup_permuto.setCurrentIndex(1)          # "On", as stored
    assert page._startup_permuto.currentIndex() == 1

    # Now the operator switches the whole subsystem off.
    page._permuto_enabled_box.setChecked(False)
    app.processEvents()

    assert page._startup_permuto.currentIndex() == 0, (
        "'Permuto at startup' did not follow the master switch")
    assert not page._startup_permuto.isEnabled()

    # ...and it reached the store, not just the widget.
    settings = QSettings("XOP", "XOPTrader")
    settings.sync()
    settings.beginGroup("startup")
    stored = str(settings.value("permuto", "off")).lower()
    settings.endGroup()
    assert stored == "off", "the corrected value never reached QSettings"


def test_a_store_that_says_on_is_corrected_when_settings_opens(
        app, monkeypatch):
    """Built disabled, with "on" already in the store: still corrected."""
    from PySide6.QtCore import QSettings

    seed = QSettings("XOP", "XOPTrader")
    seed.beginGroup("startup")
    seed.setValue("permuto", "on")
    seed.endGroup()
    seed.sync()

    page = _settings_page(monkeypatch, enabled=False)
    assert page._startup_permuto.currentIndex() == 0

    from gui.widgets.settings import load_startup_states
    assert load_startup_states()[1] == "off"


def test_the_curfew_is_greyed_but_stays_armed(app, monkeypatch):
    """The one control whose "off position" means LESS safety.

    The curfew is not an enabler -- it caps inventory carried through the
    underlying market's close, which the venue keeps matching against with a
    frozen oracle. It does nothing at all while the subsystem is off, so
    forcing it Off would buy nothing now and disarm a liquidation
    protection in the next session that actually quotes.
    """
    page = _settings_page(monkeypatch, enabled=True)
    assert page._permuto_curfew.currentIndex() == 0, "armed is index 0"

    page._permuto_enabled_box.setChecked(False)
    app.processEvents()

    assert page._permuto_curfew.currentIndex() == 0, (
        "the curfew was disarmed by the master switch")
    assert not page._permuto_curfew.isEnabled(), "it should still grey out"


def test_the_dexie_startup_row_is_untouched(app, monkeypatch):
    """A Permuto switch is not every switch."""
    page = _settings_page(monkeypatch, enabled=True)
    page._startup_dexie.setCurrentIndex(1)

    page._permuto_enabled_box.setChecked(False)
    app.processEvents()

    assert page._startup_dexie.currentIndex() == 1
    assert page._startup_dexie.isEnabled()


def test_disabling_turns_the_pages_polling_switch_off(app, monkeypatch):
    """stop_background_work stops the TIMER and leaves the button checked.

    A page reading "Stop polling" over a stopped timer is claiming to poll
    a venue it no longer talks to, and a later re-enable finds a checked
    button with nothing behind it.
    """
    window, _reads = _window(monkeypatch, enabled=True)
    page = window._unwrap(window._permuto_widget)
    try:
        page._markets_btn.setChecked(True)
        assert page._markets_btn.isChecked()

        window._on_permuto_enabled_changed(False)

        assert not page._markets_btn.isChecked(), (
            "the markets polling switch did not follow the master switch")
        assert page._markets_timer is None or not page._markets_timer.isActive()
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_the_backup_checkbox_is_never_cleared(app, monkeypatch):
    """It is a record, not a switch.

    It says the operator wrote down their recovery phrase, and it is the
    only gate between them and an unrecoverable account. "Every Permuto
    switch follows" must not reach it.
    """
    window, _reads = _window(monkeypatch, enabled=True)
    page = window._unwrap(window._permuto_widget)
    try:
        page._backup_box.setEnabled(True)
        page._backup_box.setChecked(True)

        window._on_permuto_enabled_changed(False)

        assert page._backup_box.isChecked(), (
            "the backup confirmation was cleared -- that is a safety record")
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


# --------------------------------------------------------------------------- #
# Flipping it at runtime
# --------------------------------------------------------------------------- #

def test_turning_it_off_at_runtime_hides_every_surface(app, monkeypatch):
    """OFF applies immediately, because a control that says a subsystem is
    gone has to be honest the moment it is clicked."""
    from gui.widgets import main_window as mw

    window, _reads = _window(monkeypatch, enabled=True)
    try:
        window._switch_page(mw._PAGE_PERMUTO)
        assert window._stacked.currentIndex() == mw._PAGE_PERMUTO

        window._on_permuto_enabled_changed(False)

        assert window._permuto_enabled is False
        # The ACTION, not the widget. A toolbar child of a window that was
        # never shown reports isVisible() False and isHidden() True on its
        # own, so both widget-level flags pass vacuously here. QAction
        # visibility is a plain property, so it says what was actually set
        # -- and it is also the thing that controls the toolbar slot.
        assert not window._permuto_switch_action.isVisible()
        assert not window._sidebar.is_page_visible(mw._PAGE_PERMUTO)
        # Standing on the page when it is hidden strands the operator.
        assert window._stacked.currentIndex() == mw._PAGE_DASHBOARD
        assert window._startup_permuto == "off"
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_turning_it_off_stops_the_pages_own_polling(app, monkeypatch):
    """A hidden page whose 5 s markets timer still runs is still making
    requests to a venue we compete on."""
    window, _reads = _window(monkeypatch, enabled=True)
    stopped: list[int] = []
    page = window._unwrap(window._permuto_widget)
    page.stop_background_work = lambda: stopped.append(1)
    try:
        window._on_permuto_enabled_changed(False)
        assert stopped == [1]
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_a_live_session_is_stopped_and_joined_before_anything_hides(
        app, monkeypatch):
    """Permuto orders rest at a REMOTE venue and outlive this process.

    stop() only sets a flag; the cancel runs on the worker thread and lands
    seconds later. join() is what waits for it. Hiding first would take away
    the operator's last in-process way to retract a book that is still there.
    """
    from gui.widgets import main_window as mw

    window, _reads = _window(monkeypatch, enabled=True)
    order: list[str] = []

    class _Runner:
        def stop(self):
            order.append("stop")

        def join(self, timeout_ms=30_000):
            order.append("join")

        @staticmethod
        def book_is_empty():
            order.append("check")
            return True

    window._permuto_runner = _Runner()
    monkeypatch.setattr(
        "PySide6.QtWidgets.QMessageBox.question",
        staticmethod(lambda *a, **k: __import__(
            "PySide6.QtWidgets", fromlist=["QMessageBox"]
        ).QMessageBox.StandardButton.Yes),
    )
    try:
        window._on_permuto_enabled_changed(False)

        # The stop comes first, the join waits for its cancel, and the book
        # is checked AFTER the join -- a check before it would be reading
        # the flag rather than the outcome. (The extra check in between is
        # the switch repaint the stop itself triggers.)
        assert order[0] == "stop", order
        assert "join" in order, order
        assert order[-1] == "check", order
        assert order.index("join") < len(order) - 1, order
        assert not window._sidebar.is_page_visible(mw._PAGE_PERMUTO)
    finally:
        window._permuto_runner = None
        window.close()
        window.deleteLater()
        app.processEvents()


def test_an_unconfirmed_cancel_refuses_to_hide_anything(app, monkeypatch):
    """The case the whole ordering exists for.

    The clean-stop path disarms the venue-side scheduled cancel as soon as
    its own cancel_all reports success. If the book is nonetheless not
    confirmed empty, hiding the page removes the operator's close control
    AND the net underneath it at the same moment.
    """
    from PySide6.QtWidgets import QMessageBox

    from gui.widgets import main_window as mw

    window, _reads = _window(monkeypatch, enabled=True)
    reverted: list[bool] = []

    class _StuckRunner:
        def stop(self):
            pass

        def join(self, timeout_ms=30_000):
            pass

        @staticmethod
        def book_is_empty():
            return False

    window._permuto_runner = _StuckRunner()
    window._revert_permuto_enabled = reverted.append
    monkeypatch.setattr(QMessageBox, "question",
                        staticmethod(lambda *a, **k:
                                     QMessageBox.StandardButton.Yes))
    monkeypatch.setattr(QMessageBox, "warning",
                        staticmethod(lambda *a, **k: None))
    try:
        window._on_permuto_enabled_changed(False)

        assert reverted == [True], "the operator's click was not undone"
        assert window._permuto_enabled is True
        assert window._sidebar.is_page_visible(mw._PAGE_PERMUTO)
        assert window._permuto_switch_action.isVisible()
    finally:
        window._permuto_runner = None
        window.close()
        window.deleteLater()
        app.processEvents()


def test_re_enabling_a_session_that_never_built_it_asks_for_a_restart(
        app, monkeypatch):
    """There is nothing to bring back.

    The toolbar switch and the page are built during window construction and
    the indices are positional, so a session started with Permuto off has
    nowhere to insert them. Saying "restart" is the honest answer; silently
    doing nothing is not.
    """
    from PySide6.QtWidgets import QMessageBox

    from gui.widgets import main_window as mw

    window, _reads = _window(monkeypatch, enabled=False)
    messages: list[str] = []
    window.statusBar().showMessage = lambda text, _t=0: messages.append(text)
    # A real modal blocks the offscreen run forever. Capturing it is also
    # the assertion: this is the ordinary path now that the subsystem is
    # off by default, so the dialog has to actually be raised.
    shown: list[tuple] = []
    monkeypatch.setattr(
        QMessageBox, "information",
        staticmethod(lambda *a, **k: shown.append(a)))
    try:
        assert window._permuto_built is False
        window._on_permuto_enabled_changed(True)

        # Still absent -- and the operator was told why, twice.
        assert window._permuto_switch is None
        assert not window._sidebar.is_page_visible(mw._PAGE_PERMUTO)
        assert messages and "next time" in messages[-1]
        assert shown, "the restart requirement was never shown"
        assert "next time you launch" in shown[0][2]
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_off_then_on_in_one_session_restores_the_surfaces(app, monkeypatch):
    """The surfaces were only hidden, so they can come straight back."""
    from gui.widgets import main_window as mw

    window, _reads = _window(monkeypatch, enabled=True)
    try:
        window._on_permuto_enabled_changed(False)
        assert not window._sidebar.is_page_visible(mw._PAGE_PERMUTO)

        window._on_permuto_enabled_changed(True)
        assert window._permuto_enabled is True
        assert window._permuto_switch_action.isVisible()
        assert window._sidebar.is_page_visible(mw._PAGE_PERMUTO)
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()
