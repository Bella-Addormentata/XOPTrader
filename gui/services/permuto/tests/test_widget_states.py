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
