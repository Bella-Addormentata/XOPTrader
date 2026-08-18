"""The Revive-market checkbox: per-pair opt-in to quote a dead book.

A pair whose third-party offers all fail the engine's 20% sanity filter has
an empty filtered book, and Step 7 clears its ladder every heartbeat -- the
pair is enabled, priced, and silent (wmilliETH.b/XCH was the motivating
case).  ``revive_market`` lets the engine quote from the external fair-value
anchor instead.  The C++ side is pinned in cpp/tests/test_config.cpp; these
tests pin the GUI half: the Trading Pairs checkbox and its round-trip into
the pair dict.

The round-trip matters more than the widget: ``_collect_config_dict``
rebuilds each pair dict from the table, so a checkbox with no collect wiring
would not merely be unsaved -- ticking it would do nothing, silently.

Runs headless (``QT_QPA_PLATFORM=offscreen``).
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

# Must be set before PySide6 imports a platform plugin.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402
import yaml  # noqa: E402

from PySide6.QtWidgets import QApplication, QCheckBox  # noqa: E402

from gui.widgets.settings import SettingsWidget  # noqa: E402

_REVIVE_COL = 6


@pytest.fixture(scope="session")
def qapp():
    """A single offscreen QApplication for the whole session."""
    app = QApplication.instance() or QApplication([])
    yield app


@pytest.fixture
def panel(qapp, monkeypatch):
    """A SettingsWidget with the background suggestion worker stubbed out."""
    monkeypatch.setattr(
        SettingsWidget, "_refresh_suggested_targets", lambda self: None
    )
    w = SettingsWidget()
    yield w
    w.deleteLater()


@pytest.fixture
def cfg_path(tmp_path):
    """A writable copy of the shipped example config."""
    dest = tmp_path / "config.yaml"
    shutil.copyfile(_REPO / "config.example.yaml", dest)
    return dest


def _pair_row(panel, name: str) -> int:
    for row in range(panel._pairs_table.rowCount()):
        item = panel._pairs_table.item(row, 1)
        if item is not None and item.text() == name:
            return row
    raise AssertionError(f"pair {name!r} not found in the table")


def _revive_cb(panel, row: int) -> QCheckBox:
    container = panel._pairs_table.cellWidget(row, _REVIVE_COL)
    assert container is not None, "revive cell widget missing"
    cb = container.findChild(QCheckBox)
    assert cb is not None, "revive checkbox missing"
    return cb


def _collected_pair(panel, name: str) -> dict:
    for pair in panel._collect_config_dict()["pairs"]:
        if pair.get("name") == name:
            return pair
    raise AssertionError(f"pair {name!r} not in collected config")


# --------------------------------------------------------------------------- #
# Load -> checkbox
# --------------------------------------------------------------------------- #

def test_flag_in_config_checks_the_box(panel, cfg_path):
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    for pair in raw["pairs"]:
        if pair["name"] == "wmilliETH.b/XCH":
            pair["revive_market"] = True
    cfg_path.write_text(yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_path))
    row = _pair_row(panel, "wmilliETH.b/XCH")
    assert _revive_cb(panel, row).isChecked() is True


def test_absent_flag_leaves_the_box_unchecked(panel, cfg_path):
    """Configs that predate the key must load with every box off."""
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    for pair in raw["pairs"]:
        pair.pop("revive_market", None)
    cfg_path.write_text(yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_path))
    for row in range(panel._pairs_table.rowCount()):
        assert _revive_cb(panel, row).isChecked() is False, row


# --------------------------------------------------------------------------- #
# Checkbox -> collect -> disk
# --------------------------------------------------------------------------- #

def test_ticking_writes_the_key_and_only_for_that_pair(panel, cfg_path):
    panel.load_config(str(cfg_path))
    row = _pair_row(panel, "wmilliETH.b/XCH")
    _revive_cb(panel, row).setChecked(True)

    assert _collected_pair(panel, "wmilliETH.b/XCH")["revive_market"] is True
    # No other pair picks the key up.
    for pair in panel._collect_config_dict()["pairs"]:
        if pair["name"] != "wmilliETH.b/XCH":
            assert "revive_market" not in pair, pair["name"]


def test_unticking_drops_the_key_rather_than_writing_false(panel, cfg_path):
    """The YAML stays clean: pairs that never opted in carry no key.

    An explicit ``revive_market: false`` on every pair would read as though
    someone had considered and declined revival per pair -- the absence of
    the key is the accurate statement.
    """
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    for pair in raw["pairs"]:
        if pair["name"] == "wmilliETH.b/XCH":
            pair["revive_market"] = True
    cfg_path.write_text(yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_path))
    row = _pair_row(panel, "wmilliETH.b/XCH")
    _revive_cb(panel, row).setChecked(False)

    assert "revive_market" not in _collected_pair(panel, "wmilliETH.b/XCH")


def test_round_trips_through_disk(panel, cfg_path):
    panel.load_config(str(cfg_path))
    row = _pair_row(panel, "wmilliETH.b/XCH")
    _revive_cb(panel, row).setChecked(True)
    assert panel.save_config(str(cfg_path)) is True

    on_disk = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    by_name = {p["name"]: p for p in on_disk["pairs"]}
    assert by_name["wmilliETH.b/XCH"].get("revive_market") is True

    fresh = SettingsWidget()
    try:
        fresh._refresh_suggested_targets = lambda: None  # type: ignore[method-assign]
        fresh.load_config(str(cfg_path))
        assert _revive_cb(fresh, _pair_row(fresh, "wmilliETH.b/XCH")).isChecked()
    finally:
        fresh.deleteLater()


def test_per_pair_extras_survive_the_new_column(panel, cfg_path):
    """The stashed pair dict (UserRole) still round-trips its extras.

    The new column shifted Actions from 6 to 7; this guards against the
    shift having disturbed the merge that preserves keys the table does
    not expose (overrides, stablecoin settings, ...).
    """
    panel.load_config(str(cfg_path))
    collected = _collected_pair(panel, "BYC/wUSDC.b")
    assert collected.get("is_stablecoin") is True
    assert "gamma_override" in collected


def test_ticking_dirties_the_pairs_tab(panel, cfg_path):
    panel.load_config(str(cfg_path))
    panel._clear_dirty()
    row = _pair_row(panel, "wmilliETH.b/XCH")
    _revive_cb(panel, row).setChecked(True)
    assert panel._tab_dirty.get(1) is True  # Trading Pairs = tab index 1


# --------------------------------------------------------------------------- #
# Remove-button row targeting (regression for the coordinate-frame fix)
# --------------------------------------------------------------------------- #

def test_remove_button_removes_the_clicked_row_not_row_zero(
        panel, cfg_path, qapp, monkeypatch):
    """Clicking Remove on row N must remove row N's pair.

    The old code resolved the row with ``indexAt(btn.pos())`` -- coordinates
    relative to the actions CONTAINER, always ~(4, 2) -- which maps to row 0
    for every row: Remove on any row deleted the FIRST pair, with only the
    confirmation dialog's pair name as protection.
    """
    from PySide6.QtWidgets import QMessageBox, QPushButton

    panel.load_config(str(cfg_path))
    panel.resize(1200, 700)
    panel.show()  # offscreen: realizes the table layout so indexAt works
    qapp.processEvents()

    monkeypatch.setattr(
        QMessageBox, "question",
        staticmethod(lambda *a, **k: QMessageBox.StandardButton.Yes),
    )

    def names():
        return [
            panel._pairs_table.item(r, 1).text()
            for r in range(panel._pairs_table.rowCount())
        ]

    before = names()
    assert len(before) >= 3, "example config must have several pairs"
    target = before[2]

    container = panel._pairs_table.cellWidget(2, 7)
    assert container is not None, "actions cell missing"
    btn = container.findChild(QPushButton)
    assert btn is not None
    btn.click()
    qapp.processEvents()

    after = names()
    assert target not in after, "the clicked row's pair must be removed"
    assert after[0] == before[0], "row 0 must survive a click on row 2"

    # And again after the table has shifted: rows re-resolve at click time.
    target2 = after[1]
    container2 = panel._pairs_table.cellWidget(1, 7)
    btn2 = container2.findChild(QPushButton)
    btn2.click()
    qapp.processEvents()
    final = names()
    assert target2 not in final
    assert final[0] == before[0]


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
