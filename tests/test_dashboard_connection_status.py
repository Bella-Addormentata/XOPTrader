"""The Connection Status card's string form.

`update_connection_status` accepts either a detail dict (what main_window
passes today) or a bare status string.  The string branch used a single
substring test -- "synced" in val or "conn" in val -- which paints
"Disconnected" green (it contains "conn") and "Not Synced" green (it contains
"synced").  That is alarm suppression: a green dot over a dead node.  These
tests pin the parse order, negative states first, unknown text fail-closed.

[S33 2026-09-05] SCOPE, honestly: the string branch has no production caller.
main_window.py:1002 passes a dict for all three of Full Node, Wallet and
Dexie, so no operator ever saw this green dot over a dead node -- the fix is
latent, not a live alarm being restored, and the PR description should say so.
The branch is kept as supported public API of the widget rather than deleted
(see the note on DashboardWidget.update_connection_status), and these tests
are its only exercise; that is why they exist.
"""

from __future__ import annotations

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402

from gui.widgets.dashboard import LOSS_RED, PROFIT_GREEN, WARNING  # noqa: E402


@pytest.fixture(scope="module")
def app():
    yield QApplication.instance() or QApplication(sys.argv)


def _dash():
    from gui.widgets.dashboard import DashboardWidget
    return DashboardWidget()


def _dot_style(dash, svc: str) -> str:
    return dash._conn_dots[svc][0].styleSheet()


def test_a_disconnected_string_is_not_shown_as_a_green_dot(app):
    dash = _dash()
    dash.update_connection_status({"Full Node": "Disconnected"})
    style = _dot_style(dash, "Full Node")
    assert PROFIT_GREEN not in style, (
        '"Disconnected" contains the substring "conn" -- it must not read '
        "as a healthy node"
    )
    assert LOSS_RED in style
    assert dash._conn_dots["Full Node"][1].text() == "Full Node: Disconnected"


def test_a_not_synced_string_is_not_shown_as_a_green_dot(app):
    dash = _dash()
    dash.update_connection_status({"Wallet": "Not Synced"})
    style = _dot_style(dash, "Wallet")
    assert PROFIT_GREEN not in style, (
        '"Not Synced" contains the substring "synced" -- it must not read '
        "as a synced wallet"
    )
    assert LOSS_RED in style


def test_a_not_connected_string_is_not_shown_as_a_green_dot(app):
    dash = _dash()
    dash.update_connection_status({"Dexie": "Not Connected"})
    style = _dot_style(dash, "Dexie")
    assert PROFIT_GREEN not in style
    assert LOSS_RED in style


def test_a_syncing_string_is_shown_as_a_yellow_dot(app):
    dash = _dash()
    dash.update_connection_status({"Wallet": "Wallet Syncing"})
    style = _dot_style(dash, "Wallet")
    assert WARNING in style, "an in-progress sync is neither healthy nor dead"
    assert PROFIT_GREEN not in style


def test_unrecognised_text_fails_closed(app):
    dash = _dash()
    dash.update_connection_status({"Full Node": "Ohai"})
    assert PROFIT_GREEN not in _dot_style(dash, "Full Node"), (
        "an unknown state must never read as healthy"
    )


def test_synced_and_connected_strings_stay_green(app):
    """Guards against over-correcting the fix into an all-red dot."""
    dash = _dash()
    dash.update_connection_status({"Full Node": "Synced", "Dexie": "Connected"})
    assert PROFIT_GREEN in _dot_style(dash, "Full Node")
    assert PROFIT_GREEN in _dot_style(dash, "Dexie")


def test_the_dict_and_bool_forms_are_unaffected(app):
    dash = _dash()
    dash.update_connection_status({
        "Full Node": {"colour": "yellow", "label": "Full Node: Syncing..."},
        "Wallet": False,
    })
    assert WARNING in _dot_style(dash, "Full Node")
    assert dash._conn_dots["Full Node"][1].text() == "Full Node: Syncing..."
    assert LOSS_RED in _dot_style(dash, "Wallet")
    assert dash._conn_dots["Wallet"][1].text() == "Wallet: Disconnected"
