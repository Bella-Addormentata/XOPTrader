"""Settings Save patches strategy.ratio_target_by_pair; it never rebuilds it.

[RATIO-SOT 2026-09-13] The engine reads only ``strategy.ratio_target_by_pair``.
The Settings page displayed a GUI-only per-pair mirror
(``pairs[].ratio_target_override``) whenever one existed, and every Save
REPLACED the whole map with one rebuilt from the 2-decimal Ratio column.  On
the live config of 2026-09-12 -- 0.75 mirrors left behind after the Wallet
tab's Apply wrote 0.5263157894736842 / 0.9090909090909091 -- one Save would
have reverted both live targets to 0.75 and added entries for three disabled
pairs.

These tests drive the real SettingsWidget: load_config, a table or YAML-editor
edit, save_config, then yaml.safe_load of what hit disk.  Always on tmp copies
of config.example.yaml, never the repo config.yaml: load_config merges a
sibling secrets.yaml.
"""

from __future__ import annotations

import logging
import os
import shutil
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402
import yaml  # noqa: E402

from PySide6.QtWidgets import QApplication, QMessageBox  # noqa: E402

from gui.services.config_split import load_merged, split_and_save  # noqa: E402
from gui.widgets.settings import SettingsWidget  # noqa: E402

DBX = "XCH/DBX"
BYC = "XCH/BYC"
USDC = "XCH/wUSDC.b"
DBX_TARGET = 0.5263157894736842
BYC_TARGET = 0.9090909090909091
LIVE_MAP = {DBX: DBX_TARGET, BYC: BYC_TARGET}
RATIO_COLUMN = 4
SETTINGS_LOGGER = "gui.widgets.settings"


@pytest.fixture(scope="session")
def qapp():
    """A single offscreen QApplication for the whole session."""
    app = QApplication.instance() or QApplication([])
    yield app


def _no_modal_dialog(*_args, **_kwargs):
    raise AssertionError("an unexpected modal dialog would block this test")


@pytest.fixture
def panel(qapp, monkeypatch):
    """A SettingsWidget that touches nothing outside the test's tmp dir."""
    monkeypatch.setattr(
        SettingsWidget, "_refresh_suggested_targets", lambda self: None
    )
    # save_config persists appearance preferences to the per-user QSettings
    # store the installed GUI reads; a test must never write it.
    monkeypatch.setattr(
        SettingsWidget, "_save_appearance_settings", lambda self: None
    )
    # A failed validation or write opens a modal box, which would hang an
    # offscreen run instead of failing it.
    monkeypatch.setattr(QMessageBox, "warning", staticmethod(_no_modal_dialog))
    monkeypatch.setattr(QMessageBox, "critical", staticmethod(_no_modal_dialog))
    w = SettingsWidget()
    yield w
    w.deleteLater()


@pytest.fixture
def cfg_path(tmp_path):
    """A writable copy of the shipped example config (it has no ratio map)."""
    dest = tmp_path / "config.yaml"
    shutil.copyfile(_REPO / "config.example.yaml", dest)
    return dest


@pytest.fixture
def live_cfg(cfg_path):
    """The shape of the live config.yaml on 2026-09-12.

    Stale 0.75 mirrors on XCH/BYC and XCH/DBX, a mirror with no map entry on
    a disabled XCH/wUSDC.b, and the map the Wallet tab's Apply wrote.
    """
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    names = {pair["name"] for pair in raw["pairs"]}
    assert {DBX, BYC, USDC} <= names, "fixture drift: example pairs changed"
    for pair in raw["pairs"]:
        if pair["name"] in (DBX, BYC):
            pair["ratio_target_override"] = 0.75
        if pair["name"] == DBX:
            pair["enabled"] = True
        if pair["name"] == USDC:
            pair["enabled"] = False
            pair["ratio_target_override"] = 0.6429
    raw["strategy"]["ratio_target_by_pair"] = dict(LIVE_MAP)
    cfg_path.write_text(yaml.safe_dump(raw, sort_keys=False), encoding="utf-8")
    return cfg_path


def _row(panel, name):
    table = panel._pairs_table
    for row in range(table.rowCount()):
        item = table.item(row, 1)
        if item is not None and item.text() == name:
            return row
    raise AssertionError(f"pair {name!r} is not in the table")


def _ratio_cell(panel, name):
    return panel._pairs_table.item(_row(panel, name), RATIO_COLUMN)


def _written(path):
    return yaml.safe_load(Path(path).read_text(encoding="utf-8")) or {}


def _written_map(path):
    return _written(path)["strategy"]["ratio_target_by_pair"]


def _write_out_of_band(path, **strategy_values):
    """Change strategy keys the way the Wallet tab's Apply does.

    EngineBridge.apply_wallet_allocation_targets is load_merged +
    split_and_save over the same file, while the Settings page stays open.
    A value of None removes the key, as the Apply does for empty maps.
    """
    data = load_merged(Path(path))
    strategy = data.setdefault("strategy", {})
    for key, value in strategy_values.items():
        if value is None:
            strategy.pop(key, None)
        else:
            strategy[key] = value
    split_and_save(Path(path), data)


def _edit_editor_yaml(panel, mutate):
    data = yaml.safe_load(panel._yaml_editor.toPlainText())
    mutate(data)
    panel._yaml_editor.setPlainText(yaml.safe_dump(data, sort_keys=False))


def _load_from_editor(panel, monkeypatch):
    monkeypatch.setattr(
        QMessageBox, "question",
        staticmethod(lambda *a, **k: QMessageBox.StandardButton.Yes),
    )
    panel._on_load_from_editor()


# --------------------------------------------------------------------------- #
# What the column shows
# --------------------------------------------------------------------------- #

def test_ratio_column_shows_the_engine_target_not_the_stale_mirror(
    panel, live_cfg
):
    panel.load_config(str(live_cfg))

    assert _ratio_cell(panel, DBX).text() == "52.63"
    assert _ratio_cell(panel, BYC).text() == "90.91"
    # A mirror with no map entry is not a target: that pair runs on
    # strategy.ratio_target, and a blank cell says so.
    assert _ratio_cell(panel, USDC).text() == ""


# --------------------------------------------------------------------------- #
# A Save patches the map
# --------------------------------------------------------------------------- #

def test_save_without_edits_keeps_ratio_targets_exactly(panel, live_cfg):
    panel.load_config(str(live_cfg))
    assert panel.save_config(str(live_cfg)) is True

    # Exact floats, and no entries for the disabled pairs.
    assert _written_map(live_cfg) == LIVE_MAP


def test_editing_one_cell_changes_only_that_entry(panel, live_cfg):
    panel.load_config(str(live_cfg))
    _ratio_cell(panel, DBX).setText("60")
    assert panel.save_config(str(live_cfg)) is True

    assert _written_map(live_cfg) == {DBX: 0.6, BYC: BYC_TARGET}


def test_clearing_a_cell_removes_only_that_entry(panel, live_cfg):
    panel.load_config(str(live_cfg))
    _ratio_cell(panel, BYC).setText("")
    assert panel.save_config(str(live_cfg)) is True

    assert _written_map(live_cfg) == {DBX: DBX_TARGET}


def test_retyping_the_displayed_value_is_not_an_edit(panel, live_cfg):
    panel.load_config(str(live_cfg))
    _ratio_cell(panel, DBX).setText("52.630%")
    assert panel.save_config(str(live_cfg)) is True

    # Not 0.5263: the displayed value was retyped, not changed.
    assert _written_map(live_cfg) == LIVE_MAP


def test_removing_a_pair_removes_its_ratio_entry(panel, live_cfg):
    panel.load_config(str(live_cfg))
    panel._pairs_table.removeRow(_row(panel, BYC))
    assert panel.save_config(str(live_cfg)) is True

    written = _written(live_cfg)
    assert written["strategy"]["ratio_target_by_pair"] == {DBX: DBX_TARGET}
    assert BYC not in [pair["name"] for pair in written["pairs"]]


def test_renaming_a_pair_moves_its_ratio_entry(panel, live_cfg):
    panel.load_config(str(live_cfg))
    panel._pairs_table.item(_row(panel, BYC), 1).setText("XCH/BYC2")
    assert panel.save_config(str(live_cfg)) is True

    assert _written_map(live_cfg) == {DBX: DBX_TARGET, "XCH/BYC2": BYC_TARGET}


def test_save_does_not_create_an_absent_ratio_map(panel, cfg_path):
    assert "ratio_target_by_pair" not in _written(cfg_path)["strategy"], (
        "fixture drift: the example config now ships a ratio map"
    )
    panel.load_config(str(cfg_path))
    assert panel.save_config(str(cfg_path)) is True

    assert "ratio_target_by_pair" not in _written(cfg_path)["strategy"]


def test_save_leaves_the_retired_per_pair_mirrors_untouched(panel, live_cfg):
    """[operator decision] A Save never deletes or rewrites the retired
    mirror lines, even when the column around them is edited and cleared."""
    panel.load_config(str(live_cfg))
    _ratio_cell(panel, DBX).setText("60")
    _ratio_cell(panel, BYC).setText("")
    assert panel.save_config(str(live_cfg)) is True

    mirrors = {
        pair["name"]: pair["ratio_target_override"]
        for pair in _written(live_cfg)["pairs"]
        if "ratio_target_override" in pair
    }
    assert mirrors == {USDC: 0.6429, BYC: 0.75, DBX: 0.75}


# --------------------------------------------------------------------------- #
# Writes made after the page loaded (the Wallet tab's Apply)
# --------------------------------------------------------------------------- #

def test_out_of_band_write_after_load_survives_a_save(panel, live_cfg):
    panel.load_config(str(live_cfg))
    _write_out_of_band(live_cfg, ratio_target_by_pair={DBX: 0.55, BYC: 0.8})
    assert panel.save_config(str(live_cfg)) is True

    assert _written_map(live_cfg) == {DBX: 0.55, BYC: 0.8}


def test_an_edit_wins_only_for_its_own_pair_over_an_out_of_band_write(
    panel, live_cfg, caplog
):
    panel.load_config(str(live_cfg))
    _write_out_of_band(live_cfg, ratio_target_by_pair={DBX: 0.55, BYC: 0.8})
    _ratio_cell(panel, DBX).setText("60")
    with caplog.at_level(logging.WARNING, logger=SETTINGS_LOGGER):
        assert panel.save_config(str(live_cfg)) is True

    assert _written_map(live_cfg) == {DBX: 0.6, BYC: 0.8}
    # The edit wins, but the value it replaced is never lost silently.
    conflicts = [
        record.getMessage() for record in caplog.records
        if record.levelno == logging.WARNING
        and "changed on disk" in record.getMessage()
    ]
    assert conflicts == [
        "Settings save: strategy.ratio_target_by_pair XCH/DBX: edited to "
        "0.6, replacing 0.55, which changed on disk after this page loaded "
        "0.5263157894736842"
    ]


def test_second_save_does_not_reapply_a_saved_edit_over_a_newer_disk_value(
    panel, live_cfg
):
    panel.load_config(str(live_cfg))
    _ratio_cell(panel, DBX).setText("60")
    assert panel.save_config(str(live_cfg)) is True
    assert _written_map(live_cfg)[DBX] == 0.6
    # The column now shows what hit disk.
    assert _ratio_cell(panel, DBX).text() == "60.00"

    newer = {DBX: 0.55, BYC: BYC_TARGET}
    _write_out_of_band(live_cfg, ratio_target_by_pair=newer)
    assert panel.save_config(str(live_cfg)) is True

    assert _written_map(live_cfg) == newer
    assert _ratio_cell(panel, DBX).text() == "55.00"


def test_out_of_band_wallet_apply_keys_survive_a_save(panel, cfg_path):
    shipped = _written(cfg_path)["strategy"]
    # Non-vacuity: every value written below must differ from what loads.
    assert shipped["ratio_rebalance_enabled"] is True
    for key in ("asset_target_allocations", "asset_target_tolerances",
                "ratio_band_enter_by_pair"):
        assert key not in shipped, f"fixture drift: example now ships {key}"

    panel.load_config(str(cfg_path))
    applied = {
        "ratio_rebalance_enabled": False,
        "asset_target_allocations": {"XCH": 0.6, "DBX": 0.4},
        "asset_target_tolerances": {"XCH": 0.1, "DBX": 0.1},
        "ratio_band_enter_by_pair": {DBX: 0.3},
    }
    _write_out_of_band(cfg_path, **applied)
    assert panel.save_config(str(cfg_path)) is True

    strategy = _written(cfg_path)["strategy"]
    for key, value in applied.items():
        assert strategy.get(key) == value, key


def test_a_wallet_apply_that_drops_a_key_is_not_reverted_by_a_save(
    panel, cfg_path
):
    """The Apply removes asset_target_tolerances and ratio_band_enter_by_pair
    when it has none; a save must not resurrect them from its snapshot."""
    raw = _written(cfg_path)
    raw["strategy"]["asset_target_tolerances"] = {"XCH": 0.1}
    raw["strategy"]["ratio_band_enter_by_pair"] = {DBX: 0.3}
    cfg_path.write_text(yaml.safe_dump(raw, sort_keys=False), encoding="utf-8")

    panel.load_config(str(cfg_path))
    _write_out_of_band(
        cfg_path, asset_target_tolerances=None, ratio_band_enter_by_pair=None
    )
    assert panel.save_config(str(cfg_path)) is True

    strategy = _written(cfg_path)["strategy"]
    assert "asset_target_tolerances" not in strategy
    assert "ratio_band_enter_by_pair" not in strategy


def test_collect_reflects_a_ratio_edit_without_reading_disk(panel, live_cfg):
    """_collect_config_dict runs on every edit (_mark_dirty): it merges onto
    the LOADED snapshot and never reads the file."""
    panel.load_config(str(live_cfg))
    _ratio_cell(panel, DBX).setText("60")
    _write_out_of_band(live_cfg, ratio_target_by_pair={DBX: 0.55, BYC: 0.8})

    collected = panel._collect_config_dict()["strategy"]["ratio_target_by_pair"]
    assert collected == {DBX: 0.6, BYC: BYC_TARGET}


def test_save_as_another_file_merges_onto_the_loaded_snapshot(
    panel, live_cfg, tmp_path
):
    """Only the file being saved over is re-read."""
    panel.load_config(str(live_cfg))
    _write_out_of_band(live_cfg, ratio_target_by_pair={DBX: 0.55, BYC: 0.8})
    other = tmp_path / "save-as" / "config.yaml"
    assert panel.save_config(str(other)) is True

    assert _written_map(other) == LIVE_MAP


# --------------------------------------------------------------------------- #
# Load from Editor
# --------------------------------------------------------------------------- #

def test_load_from_editor_saves_the_exact_value_not_the_rounded_cell(
    panel, live_cfg, monkeypatch
):
    panel.load_config(str(live_cfg))

    def set_dbx(data):
        data["strategy"]["ratio_target_by_pair"][DBX] = 0.61234

    _edit_editor_yaml(panel, set_dbx)
    _load_from_editor(panel, monkeypatch)
    assert _ratio_cell(panel, DBX).text() == "61.23"
    assert panel.save_config(str(live_cfg)) is True

    assert _written_map(live_cfg) == {DBX: 0.61234, BYC: BYC_TARGET}


def test_load_from_editor_saves_a_change_finer_than_the_display(
    panel, live_cfg, monkeypatch
):
    panel.load_config(str(live_cfg))

    def set_dbx(data):
        data["strategy"]["ratio_target_by_pair"][DBX] = 0.52631

    _edit_editor_yaml(panel, set_dbx)
    _load_from_editor(panel, monkeypatch)
    # Renders exactly like the value on disk ...
    assert _ratio_cell(panel, DBX).text() == "52.63"
    assert panel.save_config(str(live_cfg)) is True

    # ... and is saved anyway.
    assert _written_map(live_cfg) == {DBX: 0.52631, BYC: BYC_TARGET}


def test_load_from_editor_pair_deletion_removes_its_ratio_entry(
    panel, live_cfg, monkeypatch
):
    panel.load_config(str(live_cfg))

    def drop_byc_pair(data):
        # The pair only: the editor's map still names XCH/BYC.
        data["pairs"] = [p for p in data["pairs"] if p["name"] != BYC]

    _edit_editor_yaml(panel, drop_byc_pair)
    _load_from_editor(panel, monkeypatch)
    assert panel.save_config(str(live_cfg)) is True

    written = _written(live_cfg)
    assert written["strategy"]["ratio_target_by_pair"] == {DBX: DBX_TARGET}
    assert BYC not in [pair["name"] for pair in written["pairs"]]


# --------------------------------------------------------------------------- #
# The audit trail
# --------------------------------------------------------------------------- #

def test_save_logs_every_ratio_target_change(panel, live_cfg, caplog):
    panel.load_config(str(live_cfg))
    _ratio_cell(panel, DBX).setText("60")
    _ratio_cell(panel, BYC).setText("")
    with caplog.at_level(logging.INFO, logger=SETTINGS_LOGGER):
        assert panel.save_config(str(live_cfg)) is True

    changes = [
        record.getMessage() for record in caplog.records
        if record.levelno == logging.INFO
        and "strategy.ratio_target_by_pair" in record.getMessage()
    ]
    assert changes == [
        "Settings save: strategy.ratio_target_by_pair XCH/DBX: "
        "0.5263157894736842 -> 0.6",
        "Settings save: strategy.ratio_target_by_pair XCH/BYC: "
        "0.9090909090909091 -> (removed)",
    ]


def test_load_warns_once_about_the_retired_mirrors(panel, live_cfg, caplog):
    """[operator decision] Not purged, so named: one warning per key, not one
    per load or per save."""
    with caplog.at_level(logging.WARNING, logger=SETTINGS_LOGGER):
        panel.load_config(str(live_cfg))
        assert panel.save_config(str(live_cfg)) is True
        assert panel.save_config(str(live_cfg)) is True
        panel.load_config(str(live_cfg))

    warnings = [
        record.getMessage() for record in caplog.records
        if "ratio_target_override" in record.getMessage()
    ]
    assert len(warnings) == 1, warnings
    for fragment in (f"{USDC}=0.6429", f"{BYC}=0.75", f"{DBX}=0.75",
                     "untouched", "delete"):
        assert fragment in warnings[0], fragment


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
