"""[S74 2026-09-20] Settings Save and engine.shutdown_offers: patch one key, touch nothing else.

The Settings writer serialises the config it loaded at startup, which is how a
save on 2026-08-30 deleted a key added on disk after launch and reverted two
others. The Risk tab's new "Offers when nobody is asked" dropdown must not be
one more way to do that, and an UPGRADE must change nothing: a config with no
``engine`` section has none after an untouched save.

These tests drive the real SettingsWidget -- load_config, a dropdown change,
save_config -- and then read what hit disk, both as parsed YAML (values) and as
TEXT (key order and comments). Always on tmp copies of config.example.yaml,
never the repo config.yaml: load_config merges a sibling secrets.yaml.
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

pytest.importorskip("PySide6")

from PySide6.QtCore import QCoreApplication, QEvent  # noqa: E402
from PySide6.QtWidgets import QApplication, QMessageBox  # noqa: E402

from gui import stop_offers  # noqa: E402
from gui.services.config_split import RUAMEL_AVAILABLE  # noqa: E402
from gui.widgets.settings import SettingsWidget  # noqa: E402

#: Key order and comments survive a save only through the ruamel round-trip
#: writer; without it the writer falls back to PyYAML and says so. The VALUE
#: tests below hold either way.
requires_ruamel = pytest.mark.skipif(
    not RUAMEL_AVAILABLE, reason="ruamel.yaml not installed")

KEEP_INDEX = stop_offers.POLICIES.index("keep")
CANCEL_INDEX = stop_offers.POLICIES.index("cancel")


@pytest.fixture(scope="session")
def qapp():
    app = QApplication.instance() or QApplication([])
    yield app


def _no_modal_dialog(*_args, **_kwargs):
    raise AssertionError("an unexpected modal dialog would block this test")


@pytest.fixture
def panel(qapp, monkeypatch):
    """A SettingsWidget that touches nothing outside the test's tmp dir."""
    monkeypatch.setattr(SettingsWidget, "_refresh_suggested_targets", lambda self: None)
    monkeypatch.setattr(SettingsWidget, "_save_appearance_settings", lambda self: None)
    monkeypatch.setattr(QMessageBox, "warning", staticmethod(_no_modal_dialog))
    monkeypatch.setattr(QMessageBox, "critical", staticmethod(_no_modal_dialog))
    w = SettingsWidget()
    yield w
    # Really deleted, not deleteLater() alone: see tests/
    # test_stop_offers_window.py _destroy for what a leaked widget costs the
    # tests that run after this file.
    w.deleteLater()
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    qapp.processEvents()


def _strip_engine_section(text: str) -> str:
    """config.example.yaml without its ``engine:`` section and its comment
    block -- the shape of every config written before the key existed."""
    lines = text.splitlines(keepends=True)
    start = next(i for i, line in enumerate(lines) if line.startswith("# --- engine:"))
    end = next(i for i, line in enumerate(lines) if line.startswith("# --- depeg:"))
    assert start < end
    return "".join(lines[:start] + lines[end:])


@pytest.fixture
def old_cfg(tmp_path):
    """A config from before the key existed: no ``engine`` section at all."""
    dest = tmp_path / "config.yaml"
    text = (_REPO / "config.example.yaml").read_text(encoding="utf-8")
    dest.write_text(_strip_engine_section(text), encoding="utf-8")
    assert "engine" not in yaml.safe_load(dest.read_text(encoding="utf-8"))
    return dest


@pytest.fixture
def new_cfg(tmp_path):
    dest = tmp_path / "config.yaml"
    shutil.copyfile(_REPO / "config.example.yaml", dest)
    return dest


def _disk(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def _top_level_keys(path: Path) -> list[str]:
    """Top-level keys in FILE order -- yaml.safe_load would hide a reorder."""
    keys = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if (line and not line[0].isspace() and line[0] not in "#-"
                and ":" in line):
            keys.append(line.split(":", 1)[0])
    return keys


def _control_save(panel, path: Path) -> Path:
    """The same config, loaded and saved with the dropdown UNTOUCHED, beside
    *path*. A Settings save normalises a few things that have nothing to do
    with this key (an unticked revive_market drops its ``false``); comparing
    against this control, not against the pristine file, isolates what the
    dropdown did."""
    control_dir = path.parent / "control"
    control_dir.mkdir()
    control = control_dir / path.name
    shutil.copyfile(path, control)
    _load(panel, control)
    assert panel.save_config() is True
    return control


def _load(panel, path: Path) -> None:
    panel.load_config(str(path))
    assert Path(panel._config_path) == path.resolve(), "the config did not load"


def _touch_something_else(panel) -> None:
    """Make the page dirty through a field that has nothing to do with stops."""
    panel._q_max.setValue(panel._q_max.value() + 1)


# --------------------------------------------------------------------------- #
# The shipped example documents the key, at its default
# --------------------------------------------------------------------------- #

def test_the_example_config_documents_the_key_at_its_default():
    raw = _disk(_REPO / "config.example.yaml")
    assert raw["engine"] == {"shutdown_offers": "cancel"}
    text = (_REPO / "config.example.yaml").read_text(encoding="utf-8")
    block = text[text.index("# --- engine:"):text.index("# --- depeg:")]
    for must_say in ("offer_expiry_secs", "dead man", "log-off", "keep", "cancel"):
        assert must_say in block, f"the engine: comment block no longer mentions {must_say!r}"


# --------------------------------------------------------------------------- #
# An upgrade changes nothing
# --------------------------------------------------------------------------- #

def test_the_dropdown_shows_cancel_for_a_config_without_the_key(panel, old_cfg):
    _load(panel, old_cfg)
    assert panel._shutdown_offers.currentIndex() == CANCEL_INDEX
    assert panel._populated_shutdown_offers is None


def test_an_untouched_save_does_not_add_the_section(panel, old_cfg):
    _load(panel, old_cfg)
    _touch_something_else(panel)
    assert panel.save_config() is True

    assert "engine" not in _disk(old_cfg), (
        "a Settings save added engine.shutdown_offers to a config whose operator "
        "never touched the dropdown")


@requires_ruamel
def test_an_untouched_save_keeps_every_section_where_it_was(panel, old_cfg):
    keys_before = _top_level_keys(old_cfg)
    _load(panel, old_cfg)
    _touch_something_else(panel)
    assert panel.save_config() is True
    assert _top_level_keys(old_cfg) == keys_before


def test_loading_is_not_an_edit(panel, new_cfg):
    """Populating the dropdown must not look like the operator changing it.

    [mutation check 2026-09-20] The first version asserted ``_dirty is False``
    after loading the shipped example -- whose policy is ``cancel``, the
    dropdown's initial index, so nothing fired at all; and load_config clears
    the dirty flags afterwards anyway. With the dropdown taken out of the
    signal-blocking list it stayed green. What an unblocked dropdown really does
    is emit config_changed MID-POPULATION, with half the page still holding the
    previous config -- so load a ``keep`` config (the index moves) and count."""
    new_cfg.write_text(
        new_cfg.read_text(encoding="utf-8").replace(
            "shutdown_offers: cancel", "shutdown_offers: keep"),
        encoding="utf-8")
    emitted = []
    panel.config_changed.connect(lambda _cfg: emitted.append(1))
    _load(panel, new_cfg)

    assert panel._shutdown_offers.currentIndex() == KEEP_INDEX
    assert emitted == [], "populating the dropdown emitted config_changed"
    assert panel._dirty is False, "populating the dropdown counted as an edit"


# --------------------------------------------------------------------------- #
# A change writes one key and nothing else
# --------------------------------------------------------------------------- #

def test_choosing_keep_writes_exactly_that_key(panel, old_cfg):
    control = _control_save(panel, old_cfg)

    _load(panel, old_cfg)
    assert panel._dirty is False
    panel._shutdown_offers.setCurrentIndex(KEEP_INDEX)
    assert panel._dirty is True, "changing the dropdown must enable Save"
    assert panel.save_config() is True

    after = _disk(old_cfg)
    engine_section = after.pop("engine", None)
    assert engine_section == {"shutdown_offers": "keep"}
    assert after == _disk(control), (
        "the save changed something other than engine.shutdown_offers")


@requires_ruamel
def test_choosing_keep_appends_the_section_and_moves_nothing(panel, old_cfg):
    control = _control_save(panel, old_cfg)
    _load(panel, old_cfg)
    panel._shutdown_offers.setCurrentIndex(KEEP_INDEX)
    assert panel.save_config() is True

    # A NEW top-level section is appended; every existing line keeps its place.
    assert _top_level_keys(old_cfg) == _top_level_keys(control) + ["engine"]
    control_lines = control.read_text(encoding="utf-8").splitlines()
    saved_lines = old_cfg.read_text(encoding="utf-8").splitlines()
    assert saved_lines[:len(control_lines)] == control_lines
    assert [line for line in saved_lines[len(control_lines):] if line.strip()] == [
        "engine:", "  shutdown_offers: keep"]


def test_changing_an_existing_key_changes_only_that_value(panel, new_cfg):
    control = _control_save(panel, new_cfg)
    _load(panel, new_cfg)
    panel._shutdown_offers.setCurrentIndex(KEEP_INDEX)
    assert panel.save_config() is True

    after = _disk(new_cfg)
    assert after["engine"] == {"shutdown_offers": "keep"}
    after["engine"] = {"shutdown_offers": "cancel"}
    assert after == _disk(control)


@requires_ruamel
def test_changing_an_existing_key_keeps_its_place_and_its_comments(panel, new_cfg):
    control = _control_save(panel, new_cfg)
    _load(panel, new_cfg)
    panel._shutdown_offers.setCurrentIndex(KEEP_INDEX)
    assert panel.save_config() is True

    control_lines = control.read_text(encoding="utf-8").splitlines()
    saved_lines = new_cfg.read_text(encoding="utf-8").splitlines()
    assert len(saved_lines) == len(control_lines)
    changed = [(old, new) for old, new in zip(control_lines, saved_lines) if old != new]
    assert len(changed) == 1, f"more than the one line changed: {changed}"
    old, new = changed[0]
    assert old.split("#")[0].rstrip() == "  shutdown_offers: cancel"
    assert new.split("#")[0].rstrip() == "  shutdown_offers: keep"
    assert "#" in old and new.split("#", 1)[1] == old.split("#", 1)[1], (
        "the key's trailing comment was lost")
    # ...and the comment block that documents the section is still above it.
    text = new_cfg.read_text(encoding="utf-8")
    assert text.index("# --- engine:") < text.index("\nengine:\n")


def test_a_second_save_does_not_reapply_the_first(panel, new_cfg):
    """After a save the page's baseline is what hit disk. If the operator then
    edits the file by hand and saves the page again untouched, the hand edit
    survives."""
    _load(panel, new_cfg)
    panel._shutdown_offers.setCurrentIndex(KEEP_INDEX)
    assert panel.save_config() is True
    assert _disk(new_cfg)["engine"]["shutdown_offers"] == "keep"

    text = new_cfg.read_text(encoding="utf-8")
    new_cfg.write_text(text.replace("shutdown_offers: keep", "shutdown_offers: cancel"),
                       encoding="utf-8")
    _touch_something_else(panel)
    assert panel.save_config() is True
    assert _disk(new_cfg)["engine"]["shutdown_offers"] == "cancel"
    assert panel._shutdown_offers.currentIndex() == CANCEL_INDEX, (
        "the page still shows a value the file no longer has")


# --------------------------------------------------------------------------- #
# The lost-update shape: the file changed on disk after the page loaded
# --------------------------------------------------------------------------- #

def test_a_value_added_on_disk_after_load_survives_an_untouched_save(panel, old_cfg):
    _load(panel, old_cfg)
    with open(old_cfg, "a", encoding="utf-8") as fh:
        fh.write("\nengine:\n  shutdown_offers: keep\n")
    _touch_something_else(panel)
    assert panel.save_config() is True

    assert _disk(old_cfg)["engine"] == {"shutdown_offers": "keep"}, (
        "the save reverted an engine.shutdown_offers edited on disk after the "
        "page loaded -- the 2026-08-30 lost update, again")
    assert panel._shutdown_offers.currentIndex() == KEEP_INDEX


def test_another_key_in_the_section_survives_a_changed_dropdown(panel, new_cfg):
    """The engine section has one key today. The save must not be what makes a
    second one impossible."""
    _load(panel, new_cfg)
    text = new_cfg.read_text(encoding="utf-8")
    new_cfg.write_text(
        text.replace("  shutdown_offers: cancel",
                     "  future_key: 7\n  shutdown_offers: cancel"),
        encoding="utf-8")
    panel._shutdown_offers.setCurrentIndex(KEEP_INDEX)
    assert panel.save_config() is True

    engine = _disk(new_cfg)["engine"]
    assert engine == {"future_key": 7, "shutdown_offers": "keep"}
    assert list(engine) == ["future_key", "shutdown_offers"]


def test_an_operator_edit_beats_a_disk_edit_and_only_for_that_key(panel, new_cfg):
    _load(panel, new_cfg)                                   # disk: cancel
    panel._shutdown_offers.setCurrentIndex(KEEP_INDEX)      # operator: keep
    assert panel.save_config() is True
    assert _disk(new_cfg)["engine"]["shutdown_offers"] == "keep"

    panel._shutdown_offers.setCurrentIndex(CANCEL_INDEX)    # operator: back to cancel
    assert panel.save_config() is True
    assert _disk(new_cfg)["engine"]["shutdown_offers"] == "cancel"


# --------------------------------------------------------------------------- #
# The YAML editor path
# --------------------------------------------------------------------------- #

def test_a_policy_changed_in_the_yaml_editor_is_saved(panel, old_cfg, monkeypatch):
    """Load from Editor populates the page from the EDITOR's text. The baseline
    must stay the config on disk, or the editor's change reads as untouched and
    a Save silently drops it."""
    _load(panel, old_cfg)
    edited = _disk(old_cfg)
    edited["engine"] = {"shutdown_offers": "keep"}
    panel._yaml_editor.setPlainText(yaml.safe_dump(edited, sort_keys=False))
    monkeypatch.setattr(QMessageBox, "question", staticmethod(
        lambda *a, **k: QMessageBox.StandardButton.Yes))
    panel._on_load_from_editor()

    assert panel._shutdown_offers.currentIndex() == KEEP_INDEX
    assert panel.save_config() is True
    assert _disk(old_cfg)["engine"] == {"shutdown_offers": "keep"}
