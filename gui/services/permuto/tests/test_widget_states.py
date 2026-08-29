"""Page state machine, with the backup gate as the load-bearing test.

The gate is the whole safety argument of this page: registration is permanent
and unchangeable, so an operator must not reach it before the recovery phrase
exists somewhere off this machine. A test that only checks the happy path
would pass with the gate deleted, so the disabled cases are asserted first.
"""

from __future__ import annotations

import pytest

pytest.importorskip("PySide6")

from gui.services.permuto.identity import PermutoIdentity  # noqa: E402
from gui.services.permuto.tests.test_identity import (  # noqa: E402
    FakeProtector,
    FakeSecretsIO,
)


@pytest.fixture(scope="module")
def qapp():
    from PySide6.QtWidgets import QApplication

    app = QApplication.instance() or QApplication([])
    yield app


@pytest.fixture()
def page(qapp):
    from gui.widgets.permuto import PermutoWidget

    io = FakeSecretsIO()
    ident = PermutoIdentity(io, protector=FakeProtector())
    widget = PermutoWidget(lambda: ident)
    return widget, ident


def test_no_identity_offers_create_and_restore_only(page):
    widget, _ = page
    assert widget._create_btn.isEnabled()
    assert widget._restore_btn.isEnabled()
    assert not widget._register_btn.isEnabled()
    assert not widget._check_btn.isEnabled()
    assert "No Permuto identity" in widget._identity_lbl.text()


def test_register_is_blocked_until_the_phrase_is_confirmed(page):
    """THE gate. Delete it and this is the test that fails."""
    widget, ident = page
    ident.create()
    widget.refresh()

    assert not widget._register_btn.isEnabled()
    assert "recovery phrase" in widget._status.text().lower()

    ident.mark_backup_confirmed()
    widget.refresh()
    assert widget._register_btn.isEnabled()


def test_create_button_cannot_clobber_an_existing_identity(page):
    widget, ident = page
    ident.create()
    widget.refresh()
    assert not widget._create_btn.isEnabled()


def test_green_requires_a_verified_listing(page):
    """Green means the venue listed us, not that a call returned 200."""
    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    ident.mark_registered(
        user_id="a" * 64, trading_address="xch1test", listing_verified=True
    )
    widget.refresh()

    from gui.theme import COLORS as C

    assert "Successfully registered" in widget._status.text()
    assert C.PROFIT_GREEN.lower() in widget._status.styleSheet().lower()
    assert not widget._register_btn.isEnabled()


def test_an_unverified_registration_stays_amber_across_refresh(page):
    """THE regression. mark_registered() runs even when the board has not
    listed us yet; without a persisted verification flag the next refresh()
    promoted that unverified account to green and it never went back."""
    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    ident.mark_registered(
        user_id="a" * 64, trading_address="xch1test", listing_verified=False
    )

    from gui.theme import COLORS as C

    for _ in range(3):                      # refresh repeatedly: still amber
        widget.refresh()
        assert C.PROFIT_GREEN.lower() not in widget._status.styleSheet().lower()
        assert "not yet confirmed" in widget._status.text()


def test_verification_is_never_downgraded(page):
    """A check that lands mid-rebuild must not retract a confirmed listing."""
    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    ident.mark_registered(user_id="a" * 64, trading_address="x",
                          listing_verified=True)
    ident.mark_registered(user_id="a" * 64, trading_address="x",
                          listing_verified=False)
    assert ident.info().listing_verified is True


def test_check_button_needs_a_user_id(page):
    """Searching for "" finds nothing and rendered as 'Registered -- not on
    the leaderboard yet', claiming a registration that never happened."""
    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    widget.refresh()
    assert not widget._check_btn.isEnabled()

    ident.mark_registered(user_id="a" * 64, trading_address="x")
    widget.refresh()
    assert widget._check_btn.isEnabled()


def test_success_colour_is_actually_green(page):
    """LIGHT_GREEN in this theme is #FFB347 -- orange. Guard the mix-up."""
    from gui.theme import COLORS as C

    assert C.PROFIT_GREEN == "#00FF00"


def test_registered_but_unlisted_is_not_reported_as_success(page):
    """A rebuilt-leaderboard lag must not read as failure OR as confirmed."""
    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    widget._on_finished(
        {"ok": True, "user_id": "b" * 64, "trading_address": "xch1x",
         "listed": False, "entry": None}
    )
    from gui.theme import COLORS as C

    text = widget._status.text()
    assert "not on the leaderboard yet" in text
    assert C.PROFIT_GREEN.lower() not in widget._status.styleSheet().lower()


def test_failure_is_shown_in_red_and_survives_refresh(page):
    """refresh() rewrites the status line, so ordering matters -- the error
    must be the last thing written or it vanishes."""
    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    widget._on_finished({"ok": False, "error": "HTTP 403 signup closed"})

    from gui.theme import COLORS as C

    assert "403" in widget._status.text()
    assert C.LOSS_RED.lower() in widget._status.styleSheet().lower()


# --------------------------------------------------------------------------- #
# The permanent link, and the three windows in which it could be lost
# --------------------------------------------------------------------------- #

class _Reg:
    user_id = "d" * 64
    trading_address = "xch1linked"


def _worker(ident, action):
    from gui.widgets.permuto import _RegistrationWorker

    worker = _RegistrationWorker(ident, action)
    seen: list = []
    worker.finished.connect(seen.append)
    worker.run()
    assert seen, "the worker emitted nothing"
    return seen[0]


def test_the_link_is_persisted_before_the_leaderboard_read_back(qapp, monkeypatch):
    """119-2. auth.register() performs the PERMANENT link, but the venue
    identifiers were only written in _on_finished -- after a second request
    that blocks for up to 30 seconds per page. Closing the window in that
    window terminates the worker (stop_background_work) and the only durable
    record of a permanently linked account is gone."""
    from gui.services.permuto import auth as auth_mod

    io = FakeSecretsIO()
    ident = PermutoIdentity(io, protector=FakeProtector())
    ident.create()
    ident.mark_backup_confirmed()

    saved_when_called = {}

    def _read_back(user_id):
        saved_when_called["registered"] = ident.info().registered
        saved_when_called["user_id"] = ident.info().user_id
        raise RuntimeError("worker terminated mid-read-back")

    monkeypatch.setattr(auth_mod, "register", lambda identity: _Reg())
    monkeypatch.setattr(auth_mod, "leaderboard_entry", _read_back)

    result = _worker(ident, "register")

    assert saved_when_called["registered"] is True
    assert saved_when_called["user_id"] == "d" * 64
    assert result["ok"] is True          # verification is phase two, not fatal
    assert ident.info().trading_address == "xch1linked"


def test_a_link_that_cannot_be_saved_never_re_enables_register(page, monkeypatch):
    """119-3. The old handler said 'Do not re-register' and then called
    refresh(), which read an unregistered identity off disk and switched
    Register back on -- while pointing the operator at Check leaderboard,
    which the same refresh() had just disabled."""
    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()

    widget._on_finished({
        "ok": False, "linked": True,
        "user_id": "d" * 64, "trading_address": "xch1linked",
        "error": "secrets.yaml is read-only",
    })

    from gui.theme import COLORS as C

    assert not widget._register_btn.isEnabled()
    assert widget._recover_btn.isEnabled()
    assert C.LOSS_RED.lower() in widget._status.styleSheet().lower()

    # ...and the affordance it names actually completes the job.
    widget._on_recover()
    info = ident.info()
    assert info.registered and info.user_id == "d" * 64
    assert not widget._register_btn.isEnabled()
    assert widget._recover_btn.isHidden()


def test_an_unfinished_link_survives_a_restart_and_blocks_register(page):
    """119-4. A timeout on the commit call leaves the outcome unknown. The
    marker is durable precisely so the NEXT launch -- a fresh widget reading
    only secrets.yaml -- still refuses to link a key the venue may own."""
    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    ident.mark_link_attempt()

    widget.refresh()
    assert not widget._register_btn.isEnabled()
    assert not widget._recover_btn.isHidden()
    assert "MAY" in widget._status.text() or "may" in widget._status.text()


def test_an_indeterminate_link_is_not_reported_as_an_ordinary_failure(
    page, monkeypatch
):
    """119-4, end to end. A timeout on the commit call used to arrive as
    {ok: False} and the failure branch called refresh(), which re-enabled
    Register on backup_confirmed alone -- offering a second permanent link on
    a key the venue may already own."""
    from gui.services.permuto import auth as auth_mod
    from gui.theme import COLORS as C

    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()

    def _timed_out(identity):
        identity.mark_link_attempt()       # what auth.register() does first
        raise auth_mod.PermutoLinkIndeterminate(
            "did not complete cleanly (timed out). The key MAY already be "
            "linked -- do NOT register again."
        )

    monkeypatch.setattr(auth_mod, "register", _timed_out)
    widget._on_finished(_worker(ident, "register"))

    assert not widget._register_btn.isEnabled()
    assert not widget._recover_btn.isHidden()
    assert C.LOSS_RED.lower() in widget._status.styleSheet().lower()
    assert "Failed:" not in widget._status.text()
    assert ident.info().link_attempted is True


def test_an_unlisted_key_keeps_register_shut(page, monkeypatch):
    """[review] This asserted the opposite until the meaning of None changed.

    reconcile_registration() returns None for UNCONFIRMED, not for "never
    linked" -- the leaderboard is a positive-only oracle and an account that
    has not traded may not be listed. Clearing the marker here re-enabled
    Register for a key the venue may already own, and a second permanent
    link is the one action with no undo.

    The operator is not stranded: they can reconcile again later, or discard
    the identity deliberately, which is still permitted because a link
    ATTEMPT is not a registration.
    """
    from gui.services.permuto import auth as auth_mod

    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    ident.mark_link_attempt()

    monkeypatch.setattr(auth_mod, "reconcile_registration", lambda i: None)
    widget._on_finished(_worker(ident, "reconcile"))

    assert ident.info().link_attempted is True, "the attempt marker was lost"
    assert not widget._register_btn.isEnabled()
    assert "not proof" in widget._status.text()


def test_an_unresolved_attempt_can_still_be_discarded_deliberately(page):
    """The escape hatch, so keeping the marker is not a trap."""
    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    ident.mark_link_attempt()

    ident.discard_unregistered()          # must not raise
    assert not ident.exists()


def test_reconciling_an_already_linked_key_records_it_without_re_linking(
    page, monkeypatch
):
    from gui.services.permuto import auth as auth_mod

    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    ident.mark_link_attempt()

    monkeypatch.setattr(auth_mod, "reconcile_registration",
                        lambda i: ("d" * 64, "xch1linked"))
    monkeypatch.setattr(auth_mod, "leaderboard_entry", lambda uid: None)
    widget._on_finished(_worker(ident, "reconcile"))

    info = ident.info()
    assert info.registered and info.user_id == "d" * 64
    assert info.link_attempted is False
    assert not widget._register_btn.isEnabled()


def test_a_transient_leaderboard_error_never_retracts_a_confirmed_listing(
    page, monkeypatch
):
    """119-6. The register branch demoted a leaderboard failure to
    verify_error; the check branch did not, so the same PermutoAuthError came
    back as ok=False and the red 'Failed: HTTP 503' overwrote the green that
    refresh() had just painted from the persisted flag. Check is enabled for
    a verified operator and every amber message tells them to press it."""
    from gui.services.permuto import auth as auth_mod
    from gui.services.permuto.auth import PermutoAuthError
    from gui.theme import COLORS as C

    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    ident.mark_registered(user_id="d" * 64, trading_address="xch1linked",
                          listing_verified=True)

    def _rebuilding(user_id):
        raise PermutoAuthError("GET /exchange/leaderboard -> HTTP 503")

    monkeypatch.setattr(auth_mod, "leaderboard_entry", _rebuilding)
    widget._on_finished(_worker(ident, "check"))

    assert C.LOSS_RED.lower() not in widget._status.styleSheet().lower()
    assert C.PROFIT_GREEN.lower() in widget._status.styleSheet().lower()
    assert ident.info().listing_verified is True
    # The empty-category parenthetical: entry is None on this path, so the
    # green line used to end "as dddddddddddddddd ()".
    assert "()" not in widget._status.text()


def test_a_genuine_check_failure_is_still_reported(page, monkeypatch):
    """Demoting the read-back must not swallow a real problem."""
    from gui.theme import COLORS as C

    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()

    class Broken:
        def info(self):
            raise RuntimeError("secrets.yaml is corrupt")

    widget._on_finished(_worker(Broken(), "check"))
    assert C.LOSS_RED.lower() in widget._status.styleSheet().lower()
    assert "corrupt" in widget._status.text()


# --------------------------------------------------------------------------- #
# The recovery-phrase modal
# --------------------------------------------------------------------------- #

def test_the_word_grid_cannot_make_an_untracked_clipboard_copy(qapp):
    """119-5. The dialog clears the clipboard on close, but only for copies
    it made itself -- it compares digests so it never wipes something the
    operator copied afterwards. A read-only QTextEdit still offers Select All
    / Copy and mouse selection, so Ctrl+C on the grid put the full mnemonic on
    the clipboard without ever entering _copy(); the digest guard then
    short-circuited and the promise on screen ('Cleared when this dialog
    closes') became false. base_wallet.py closes exactly this hole."""
    from PySide6.QtCore import Qt

    from gui.widgets.permuto import _RecoveryPhraseDialog

    phrase = " ".join(["abandon"] * 23 + ["art"])
    dialog = _RecoveryPhraseDialog(phrase)
    try:
        assert dialog._grid.contextMenuPolicy() == \
            Qt.ContextMenuPolicy.NoContextMenu
        assert dialog._grid.focusPolicy() == Qt.FocusPolicy.NoFocus
        # Mouse selection is what populates the X11/Wayland PRIMARY buffer,
        # which clipboard.clear() does not touch at all.
        assert dialog._grid.textInteractionFlags() == \
            Qt.TextInteractionFlag.NoTextInteraction
    finally:
        dialog.deleteLater()


def test_a_tracked_copy_is_still_cleared_when_the_dialog_closes(qapp):
    from PySide6.QtWidgets import QApplication

    from gui.widgets.permuto import _RecoveryPhraseDialog

    phrase = " ".join(["abandon"] * 23 + ["art"])
    dialog = _RecoveryPhraseDialog(phrase)
    try:
        dialog._copy(phrase)
        clipboard = QApplication.clipboard()
        assert clipboard.text() == phrase
        dialog.reject()                      # Escape and the X take this path
        assert clipboard.text() == ""
    finally:
        dialog.deleteLater()


def test_public_key_is_shown_and_private_key_is_not(page):
    widget, ident = page
    pubkey, phrase = ident.create()
    widget.refresh()

    shown = widget._identity_lbl.text()
    assert pubkey in shown
    assert bytes(ident.private_key()).hex() not in shown
    for word in phrase.split():
        assert " %s " % word not in shown


def test_page_constants_match_the_sidebar_order(qapp):
    """Inserting a page shifts every index after it.

    _PAGE_SETTINGS stayed 9 when Permuto took index 9, so the Settings menu,
    open_settings_page() and the first-run missing-config redirect all opened
    Permuto -- a new user with no config landed on a key-generation page.
    Nothing tied the constants to the sidebar, so nothing caught it. This does.
    """
    from gui.widgets import main_window as mw
    from gui.widgets.sidebar import _NAV_ITEMS

    expected = {
        "Dashboard": mw._PAGE_DASHBOARD,
        "Charts": mw._PAGE_CHARTS,
        "Orders": mw._PAGE_ORDERS,
        "Order Book": mw._PAGE_ORDER_BOOK,
        "Analysis": mw._PAGE_ANALYSIS,
        "Wallet": mw._PAGE_WALLET,
        "Reports": mw._PAGE_REPORTS,
        "Warp": mw._PAGE_WARP,
        "Base": mw._PAGE_BASE_WALLET,
        "Permuto": mw._PAGE_PERMUTO,
        "Settings": mw._PAGE_SETTINGS,
    }
    labels = [label for label, _icon in _NAV_ITEMS]
    assert len(labels) == len(expected), "a nav item has no page constant"
    for label, index in expected.items():
        assert labels[index] == label, (
            "_PAGE_* constant for %s points at %s" % (label, labels[index])
        )


# --------------------------------------------------------------------------- #
# Sections
#
# The page grew a sub-menu that opens inside the tab. The load-bearing checks
# are that nothing about the identity state machine moved, and that the two
# new surfaces which touch the network or the account are gated.
# --------------------------------------------------------------------------- #

def _await_poll(widget, qapp, timeout_ms: int = 5000) -> None:
    """Drive the event loop until the market worker has reported.

    The poll is on a worker thread now, so a test that asserts immediately
    after enabling it is racing the socket -- which is the whole point of the
    change.
    """
    from PySide6.QtCore import QDeadlineTimer

    deadline = QDeadlineTimer(timeout_ms)
    while widget._markets_thread is not None and not deadline.hasExpired():
        qapp.processEvents()
    qapp.processEvents()


def _fake_market_page(qapp, reader):
    from gui.widgets.permuto import PermutoWidget

    io = FakeSecretsIO()
    ident = PermutoIdentity(io, protector=FakeProtector())
    return PermutoWidget(lambda: ident, market_reader=reader), ident


def test_the_page_opens_on_identity(page):
    widget, _ = page
    assert widget._sections.current_index() == 0
    assert widget._sections.bar.title(0) == "Identity"


def test_every_section_is_reachable(page):
    widget, _ = page
    titles = [widget._sections.bar.title(i)
              for i in range(widget._sections.bar.count())]
    assert titles == ["Identity", "Markets", "Quoting", "Activity"]
    for i in range(len(titles)):
        widget._sections.set_current(i)
        assert widget._sections.stack.currentIndex() == i


def test_the_status_line_is_outside_the_sections(page):
    """It reports link attempts, which an operator must not miss.

    Burying it in one section would hide the most important line on the page
    behind a click.
    """
    widget, _ = page
    for i in range(widget._sections.bar.count()):
        widget._sections.set_current(i)
        assert widget._status.parentWidget() is widget


# -- Markets: read-only, and it stops when you look away --------------------- #

def test_markets_polls_only_while_its_section_is_open(qapp):
    calls = []

    def reader():
        calls.append(1)
        return {"prices": {"QQQ-VOL-PERP": 0.07}, "trading_paused": False}

    widget, _ = _fake_market_page(qapp, reader)
    widget._sections.set_current(1)
    widget._markets_btn.setChecked(True)
    _await_poll(widget, qapp)
    assert calls, "polling did not start"

    # Navigating away must stop it: a background poll feeding a widget nobody
    # is looking at is a request every 5s against a venue we compete on.
    widget._sections.set_current(0)
    assert not widget._markets_btn.isChecked()


def test_a_failing_poll_does_not_raise_into_the_event_loop(qapp):
    def boom():
        raise RuntimeError("venue down")

    widget, _ = _fake_market_page(qapp, boom)
    widget._sections.set_current(1)
    widget._markets_btn.setChecked(True)          # must not raise
    _await_poll(widget, qapp)
    assert "unavailable" in widget._markets_lbl.text()
    assert "venue down" in widget._markets_note.text()


def test_a_venue_pause_is_stated_plainly(qapp):
    widget, _ = _fake_market_page(
        qapp,
        lambda: {"prices": {"QQQ-VOL-PERP": 0.07}, "trading_paused": True},
    )
    widget._sections.set_current(1)
    widget._markets_btn.setChecked(True)
    _await_poll(widget, qapp)
    assert "PAUSED" in widget._markets_note.text()


# -- Quoting: never armed by accident ---------------------------------------- #

def test_quoting_is_not_armed_without_a_registration(page):
    widget, _ = page
    widget._sections.set_current(2)
    assert not widget._arm_btn.isEnabled()
    assert "not registered" in widget._arm_note.text()
    assert "toolbar" in widget._arm_note.text()


def test_quoting_stays_unarmed_even_once_registered(page):
    """Arming places REAL orders with real collateral.

    That is a deliberate decision, not a side effect of opening a tab, so the
    button stays disabled and the page says why rather than implying it with
    a grey rectangle.
    """
    widget, ident = page
    ident.create()
    ident.mark_backup_confirmed()
    ident.mark_registered(user_id="u" * 64, trading_address="xch1example")

    widget._sections.set_current(2)
    # The page describes; the toolbar switch arms. One control, not two.
    assert not widget._arm_btn.isEnabled()
    assert "PERMUTO switch" in widget._arm_note.text()
    assert "cancels every resting order" in widget._arm_note.text()


def test_the_quoting_summary_states_the_ring_and_the_risk_lines(page):
    widget, _ = page
    widget._sections.set_current(2)
    text = widget._quoting_lbl.text()
    assert "2.00%" in text          # the ring depth credit is measured in
    assert "5.00%" in text          # the legal band
    assert "utilisation" in text


def test_the_market_poll_does_not_run_on_the_gui_thread(qapp):
    """A slow venue must not freeze the application.

    The default reader makes two requests at 30s timeouts on a 5s timer, so
    running it inline froze the whole UI for up to a minute per poll.
    """
    import threading

    seen = {}

    def reader():
        seen["thread"] = threading.current_thread().name
        return {"prices": {"QQQ-VOL-PERP": 0.07}, "trading_paused": False}

    widget, _ = _fake_market_page(qapp, reader)
    widget._sections.set_current(1)
    widget._markets_btn.setChecked(True)
    _await_poll(widget, qapp)
    assert seen.get("thread") != threading.main_thread().name


def test_overlapping_polls_are_refused_rather_than_queued(qapp):
    """The timer fires every 5s and a request may take 30."""
    widget, _ = _fake_market_page(
        qapp, lambda: {"prices": {"Q": 1.0}, "trading_paused": False})
    widget._sections.set_current(1)
    widget._poll_markets()
    first = widget._markets_thread
    widget._poll_markets()                 # must not start a second
    assert widget._markets_thread is first
    _await_poll(widget, qapp)


def test_teardown_joins_the_market_thread_too(qapp):
    """A page that joins one of two threads still aborts, just less often."""
    widget, _ = _fake_market_page(
        qapp, lambda: {"prices": {"Q": 1.0}, "trading_paused": False})
    widget._sections.set_current(1)
    widget._markets_btn.setChecked(True)
    widget.stop_background_work()
    assert widget._markets_thread is None


# --------------------------------------------------------------------------- #
# [review] The way out of an unfinished link
# --------------------------------------------------------------------------- #

def _restored(page):
    """Reproduce the trap: a never-registered phrase restored on a FRESH
    machine.

    The phrase comes from a throwaway identity so the test needs no wordlist
    dependency, and it is restored into a brand-new store -- which is exactly
    the "moved to a new install" case. restore() sets the attempt marker
    unconditionally, so the page opens with Register shut, Create shut and
    only Recover offered, and Recover asks a positive-only oracle that can
    never answer "no".
    """
    from gui.widgets.permuto import PermutoWidget

    _, source = page
    _pub, phrase = source.create()

    fresh = PermutoIdentity(FakeSecretsIO(), protector=FakeProtector())
    fresh.restore(phrase)
    widget = PermutoWidget(lambda: fresh)
    widget.refresh()
    return widget, fresh


def test_a_restored_never_registered_phrase_lands_in_the_unfinished_state(page):
    widget, ident = _restored(page)
    info = ident.info()
    assert not info.registered
    assert info.link_attempted, "the marker that shuts Register"
    assert not widget._register_btn.isEnabled()


def test_discard_is_hidden_until_the_venue_has_been_asked(page):
    """Throwing a key away is not a first move: the operator must have
    consulted the leaderboard before being allowed to."""
    widget, _ = _restored(page)
    assert not widget._discard_btn.isEnabled()


def test_discard_appears_once_a_reconcile_could_not_resolve_the_link(page):
    """Without this the operator is trapped -- Register shut, Create shut,
    Recover unable to answer -- with registration closing Monday 17:00 ET.
    The status text already said "discard this identity deliberately" and
    nothing in the page could do it."""
    widget, _ = _restored(page)
    widget._reconcile_unresolved = True
    widget.refresh()
    assert widget._discard_btn.isEnabled()


def test_discard_refuses_without_the_typed_word(page, monkeypatch):
    """A Yes/No box is answered reflexively, and discarding a key that IS
    linked abandons the account permanently."""
    from PySide6.QtWidgets import QInputDialog
    widget, ident = _restored(page)
    before = ident.info().pubkey

    monkeypatch.setattr(QInputDialog, "getText",
                        staticmethod(lambda *a, **k: ("yes", True)))
    widget._on_discard()
    assert ident.info().pubkey == before, "discarded without the word"


def test_discard_removes_the_identity_when_confirmed(page, monkeypatch):
    from PySide6.QtWidgets import QInputDialog

    from gui.services.permuto.identity import PermutoIdentityError

    widget, ident = _restored(page)
    assert ident.info().pubkey
    monkeypatch.setattr(QInputDialog, "getText",
                        staticmethod(lambda *a, **k: ("DISCARD", True)))
    widget._on_discard()

    # "No identity" is an absence, not an empty record -- info() says so.
    with pytest.raises(PermutoIdentityError):
        ident.info()
    # And the page recovers into the create/restore state rather than raising.
    assert widget._create_btn.isEnabled()


def test_discard_still_refuses_a_registered_identity(page, monkeypatch):
    """The backstop underneath the two UI guards: after linking, the key IS
    the account, and only the 24 words could bring it back."""
    from PySide6.QtWidgets import QInputDialog
    widget, ident = _restored(page)
    ident.mark_registered(user_id="u1", trading_address="xch1abc")
    before = ident.info().pubkey

    monkeypatch.setattr(QInputDialog, "getText",
                        staticmethod(lambda *a, **k: ("DISCARD", True)))
    widget._on_discard()
    assert ident.info().pubkey == before, "a registered identity was discarded"
