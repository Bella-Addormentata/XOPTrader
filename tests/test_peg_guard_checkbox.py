"""The Peg guard checkbox: per-pair opt-out of the peg gates for revival.

[PEGGUARD 2026-10-22] Ticked by default. Unticking it lets a revive pair
keep quoting through a peg suspension or depeg bail-out (the engine ignores
it unless Revive is also ticked; the C++ side is pinned in
cpp/tests/test_config.cpp and the engine's peg_guard_is_waived). These
tests pin the GUI half: the column, its default, and its round-trip into
the pair dict -- only an unticked box is written, as ``peg_guard: false``.

Runs headless (``QT_QPA_PLATFORM=offscreen``).
"""

from __future__ import annotations

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

from PySide6.QtWidgets import QApplication, QCheckBox  # noqa: E402

from gui.widgets.settings import SettingsWidget  # noqa: E402

_GUARD_COL = 7


@pytest.fixture(scope="session")
def qapp():
    app = QApplication.instance() or QApplication([])
    yield app


@pytest.fixture
def panel(qapp, monkeypatch):
    monkeypatch.setattr(
        SettingsWidget, "_refresh_suggested_targets", lambda self: None
    )
    w = SettingsWidget()
    yield w
    w.deleteLater()


@pytest.fixture
def cfg_path(tmp_path):
    dest = tmp_path / "config.yaml"
    shutil.copyfile(_REPO / "config.example.yaml", dest)
    return dest


def _pair_row(panel, name: str) -> int:
    for row in range(panel._pairs_table.rowCount()):
        item = panel._pairs_table.item(row, 1)
        if item is not None and item.text() == name:
            return row
    raise AssertionError(f"pair {name!r} not found in the table")


def _guard_cb(panel, row: int) -> QCheckBox:
    container = panel._pairs_table.cellWidget(row, _GUARD_COL)
    assert container is not None, "peg guard cell widget missing"
    cb = container.findChild(QCheckBox)
    assert cb is not None, "peg guard checkbox missing"
    return cb


def _collected_pair(panel, name: str) -> dict:
    for pair in panel._collect_config_dict()["pairs"]:
        if pair.get("name") == name:
            return pair
    raise AssertionError(f"pair {name!r} not in collected config")


def test_header_names_the_column(panel):
    header = panel._pairs_table.horizontalHeaderItem(_GUARD_COL)
    assert header is not None and header.text() == "Peg guard"
    assert panel._pairs_table.horizontalHeaderItem(_GUARD_COL + 1).text() == "Actions"


def test_absent_key_loads_ticked(panel, cfg_path):
    """Every pair is guarded unless its config says otherwise."""
    panel.load_config(str(cfg_path))
    for row in range(panel._pairs_table.rowCount()):
        assert _guard_cb(panel, row).isChecked() is True, row


def test_false_in_config_unticks_the_box(panel, cfg_path):
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    for pair in raw["pairs"]:
        if pair["name"] == "BYC/wUSDC.b":
            pair["peg_guard"] = False
    cfg_path.write_text(yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_path))
    assert _guard_cb(panel, _pair_row(panel, "BYC/wUSDC.b")).isChecked() is False
    assert _guard_cb(panel, _pair_row(panel, "XCH/wUSDC.b")).isChecked() is True


def test_unticking_writes_false_for_that_pair_only(panel, cfg_path):
    panel.load_config(str(cfg_path))
    _guard_cb(panel, _pair_row(panel, "BYC/wUSDC.b")).setChecked(False)

    assert _collected_pair(panel, "BYC/wUSDC.b")["peg_guard"] is False
    for pair in panel._collect_config_dict()["pairs"]:
        if pair["name"] != "BYC/wUSDC.b":
            assert "peg_guard" not in pair, pair["name"]


def test_reticking_drops_the_key(panel, cfg_path):
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    for pair in raw["pairs"]:
        if pair["name"] == "BYC/wUSDC.b":
            pair["peg_guard"] = False
    cfg_path.write_text(yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_path))
    _guard_cb(panel, _pair_row(panel, "BYC/wUSDC.b")).setChecked(True)
    assert "peg_guard" not in _collected_pair(panel, "BYC/wUSDC.b")


def test_round_trips_through_disk(panel, cfg_path):
    panel.load_config(str(cfg_path))
    _guard_cb(panel, _pair_row(panel, "BYC/wUSDC.b")).setChecked(False)
    assert panel.save_config(str(cfg_path)) is True

    on_disk = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    by_name = {p["name"]: p for p in on_disk["pairs"]}
    assert by_name["BYC/wUSDC.b"].get("peg_guard") is False

    fresh = SettingsWidget()
    try:
        fresh._refresh_suggested_targets = lambda: None  # type: ignore[method-assign]
        fresh.load_config(str(cfg_path))
        assert _guard_cb(fresh, _pair_row(fresh, "BYC/wUSDC.b")).isChecked() is False
    finally:
        fresh.deleteLater()


def test_unticking_dirties_the_pairs_tab(panel, cfg_path):
    panel.load_config(str(cfg_path))
    panel._clear_dirty()
    _guard_cb(panel, _pair_row(panel, "BYC/wUSDC.b")).setChecked(False)
    assert panel._tab_dirty.get(1) is True  # Trading Pairs = tab index 1


def test_tooltip_says_it_needs_revive(panel, cfg_path):
    panel.load_config(str(cfg_path))
    tip = _guard_cb(panel, _pair_row(panel, "BYC/wUSDC.b")).toolTip()
    assert "Revive" in tip


def _set_pair_key(cfg_path, name: str, line: str) -> None:
    """Set one key of one pair as a raw YAML line, bypassing safe_dump so
    null and quoted spellings reach the loader exactly as a hand-edited
    file has them. Replaces the key if the pair already has it."""
    lines = cfg_path.read_text(encoding="utf-8").splitlines()
    at = next((i for i, ln in enumerate(lines)
               if ln.strip().lstrip("- ").strip() in (f'name: "{name}"', f"name: {name}")),
              None)
    assert at is not None, f"pair {name!r} not found in the example config"
    indent = " " * (len(lines[at]) - len(lines[at].lstrip(" -")))
    key = line.split(":", 1)[0]
    end = at + 1
    while end < len(lines) and (lines[end].startswith(indent) or not lines[end].strip()):
        end += 1
    for i in range(at - 1, -1, -1):          # the entry may start above `name`
        if lines[i].lstrip().startswith("- "):
            start = i
            break
    else:
        start = at
    for i in range(start, end):
        if lines[i].strip().lstrip("- ").startswith(f"{key}:"):
            lines[i] = lines[i][: len(lines[i]) - len(lines[i].lstrip(" -"))] + line
            break
    else:
        lines.insert(at + 1, indent + line)
    cfg_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


@pytest.mark.parametrize("spelling", ["peg_guard:", "peg_guard: null", "peg_guard: ~"])
def test_a_null_value_keeps_the_guard_on_through_a_save(panel, cfg_path, spelling):
    """The engine skips a null peg_guard and keeps it on; the GUI must too,
    or saving would write peg_guard: false and silently disable it."""
    _set_pair_key(cfg_path, "BYC/wUSDC.b", spelling)
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    assert {p["name"]: p for p in raw["pairs"]}["BYC/wUSDC.b"]["peg_guard"] is None

    panel.load_config(str(cfg_path))
    assert _guard_cb(panel, _pair_row(panel, "BYC/wUSDC.b")).isChecked() is True
    assert panel.save_config(str(cfg_path)) is True
    on_disk = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    assert {p["name"]: p for p in on_disk["pairs"]}["BYC/wUSDC.b"].get("peg_guard") \
        is not False


@pytest.mark.parametrize("spelling,expected", [
    ('peg_guard: "false"', False), ("peg_guard: 'no'", False),
    ('peg_guard: "true"', True), ('peg_guard: "garbage"', True),
])
def test_quoted_spellings_read_as_the_engine_reads_them(panel, cfg_path,
                                                       spelling, expected):
    _set_pair_key(cfg_path, "BYC/wUSDC.b", spelling)
    panel.load_config(str(cfg_path))
    assert _guard_cb(panel, _pair_row(panel, "BYC/wUSDC.b")).isChecked() is expected


def test_the_same_reading_covers_enabled_and_revive(panel, cfg_path):
    """Same defect class: ``enabled: null`` is enabled to the engine and a
    quoted ``revive_market: "false"`` is off."""
    _set_pair_key(cfg_path, "BYC/wUSDC.b", "enabled: null")
    _set_pair_key(cfg_path, "BYC/wUSDC.b", 'revive_market: "false"')
    panel.load_config(str(cfg_path))
    row = _pair_row(panel, "BYC/wUSDC.b")

    def _cb(col):
        return panel._pairs_table.cellWidget(row, col).findChild(QCheckBox)

    assert _cb(0).isChecked() is True, "enabled: null is enabled"
    assert _cb(6).isChecked() is False, 'revive_market: "false" is off'
