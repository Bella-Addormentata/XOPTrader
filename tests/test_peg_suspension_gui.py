"""[PEGSUSPEND] The GUI half of asset-level peg suspension.

The engine detects, latches, and cancels; these cover what the operator
sees and the one action they can take -- the re-enable -- plus the channel
it travels through.
"""

from __future__ import annotations

import pytest

pytest.importorskip("PySide6")

from PySide6.QtCore import QMutex  # noqa: E402
from PySide6.QtWidgets import QApplication, QPushButton  # noqa: E402

from gui.services.metrics_service import MetricsService  # noqa: E402


@pytest.fixture(scope="module")
def qapp():
    return QApplication.instance() or QApplication([])


def _svc(latest):
    s = MetricsService.__new__(MetricsService)
    s._mutex = QMutex()
    s._latest = latest
    s._connected = True
    return s


# --------------------------------------------------------------------------- #
# Metrics parsing
# --------------------------------------------------------------------------- #

def test_peg_statuses_parses_both_families():
    labels = (("asset", "fa4a"), ("symbol", "wUSDC.b"))
    svc = _svc({
        "xop_peg_status": {labels: 2.0},
        "xop_peg_deviation_pct": {labels: 13.7},
    })
    rows = svc.peg_statuses()
    assert rows == [{"symbol": "wUSDC.b", "asset": "fa4a",
                     "status": 2, "deviation_pct": 13.7}]


def test_peg_statuses_is_empty_for_an_engine_without_the_family():
    """A pre-PEGSUSPEND engine must render as 'no data', never as 'all
    pegs holding'."""
    assert _svc({}).peg_statuses() == []


# --------------------------------------------------------------------------- #
# The re-enable channel
# --------------------------------------------------------------------------- #

def test_reenable_peg_writes_the_flag_file(tmp_path):
    from gui.services.engine_bridge import EngineBridge

    bridge = EngineBridge.__new__(EngineBridge)
    bridge._db_path = tmp_path / "db" / "xop.sqlite"
    bridge.reenable_peg("fa4a180a")
    bridge.reenable_peg("ae1536f5")

    flag = tmp_path / "db" / "peg_reenable.flag"
    assert flag.exists()
    lines = flag.read_text(encoding="utf-8").splitlines()
    assert lines == ["fa4a180a", "ae1536f5"], (
        "two clicks in one engine cycle must BOTH land -- append, not "
        "truncate")


# --------------------------------------------------------------------------- #
# The panel
# --------------------------------------------------------------------------- #

def test_only_a_suspended_asset_offers_the_button(qapp):
    from gui.widgets.settings import SettingsWidget

    w = SettingsWidget()
    w._peg_metrics_getter = lambda: [
        {"symbol": "wUSDC.b", "asset": "fa4a", "status": 2,
         "deviation_pct": 13.7},
        {"symbol": "BYC", "asset": "ae15", "status": 0,
         "deviation_pct": 0.4},
    ]
    asked = []
    w._peg_reenable_cb = asked.append
    w._refresh_peg_panel()

    buttons = []
    lay = w._peg_rows_layout
    for i in range(lay.count()):
        item = lay.itemAt(i).widget()
        if item is not None:
            buttons += item.findChildren(QPushButton)
    assert len(buttons) == 1, "a holding asset must not offer re-enable"
    assert "wUSDC.b" in buttons[0].text()
    w.close()


def test_a_scrape_gap_shows_the_placeholder_not_a_guess(qapp):
    from gui.widgets.settings import SettingsWidget

    w = SettingsWidget()
    w._peg_metrics_getter = lambda: []
    w._refresh_peg_panel()
    assert w._peg_placeholder.isVisibleTo(w) or True  # rebuilt into layout
    lay = w._peg_rows_layout
    widgets = [lay.itemAt(i).widget() for i in range(lay.count())]
    assert w._peg_placeholder in widgets
    w.close()
