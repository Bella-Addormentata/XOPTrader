"""The drawdown key rename: the GUI must never write the fatal legacy name.

[DRAWDOWN-EQUITY 2026-08-04 / incident 2026-08-18] The engine renamed
``risk.max_drawdown_pct`` to ``risk.max_drawdown_frac`` and treats the OLD
key as a hard startup error.  The Settings save dict kept writing the old
name for two weeks; the first GUI save after the rename -- ticking Revive
during the v0.9.9 upgrade -- poisoned config.yaml and the new engine
refused to start, mid-upgrade, with the trading bot down.

Three rails, each of which independently prevented recurrence being enough:
the save writes the NEW key only; the load reads the new key (with a
migration fallback); and the deep-merge resurrection path purges a legacy
key inherited from a previously poisoned on-disk snapshot.
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

from PySide6.QtWidgets import QApplication  # noqa: E402

from gui.widgets.settings import SettingsWidget  # noqa: E402


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


def test_collect_preserves_strategy_keys_the_ui_does_not_own(panel, cfg_path):
    """The comment-preserving writer deletes keys absent from the collected
    dict, so any strategy key without a Settings widget used to die on the
    first save -- including an operator's emergency
    xch_cycle_commit_frac: 0.0 reverting to the engine default (review on
    the coin-lock-ledger PR).  The collector now passes through every
    loaded strategy key it does not own."""
    import yaml as _yaml

    raw = _yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    raw.setdefault("strategy", {})["xch_cycle_commit_frac"] = 0.0
    raw["strategy"]["some_future_knob"] = "kept"
    cfg_path.write_text(_yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_path))
    strategy = panel._collect_config_dict()["strategy"]

    assert strategy["xch_cycle_commit_frac"] == 0.0
    assert strategy["some_future_knob"] == "kept"
    # Widget-owned keys still come from the widgets, not the snapshot.
    assert "gamma" in strategy


def test_collect_writes_the_new_key_and_never_the_fatal_one(panel, cfg_path):
    panel.load_config(str(cfg_path))
    risk = panel._collect_config_dict()["risk"]
    assert "max_drawdown_frac" in risk
    assert "max_drawdown_pct" not in risk, (
        "the engine hard-errors at startup on the legacy key; writing it "
        "bricks the next engine launch"
    )


def test_load_reads_the_new_key(panel, cfg_path):
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    raw.setdefault("risk", {})["max_drawdown_frac"] = 0.25
    raw["risk"].pop("max_drawdown_pct", None)
    cfg_path.write_text(yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_path))
    assert panel._max_drawdown_pct.value() == pytest.approx(0.25)


def test_load_falls_back_to_the_legacy_key_for_old_configs(panel, cfg_path):
    """A pre-rename config still populates the widget (migration path)."""
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    raw.setdefault("risk", {}).pop("max_drawdown_frac", None)
    raw["risk"]["max_drawdown_pct"] = 0.33
    cfg_path.write_text(yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_path))
    assert panel._max_drawdown_pct.value() == pytest.approx(0.33)


def test_save_purges_a_poisoned_snapshot(panel, cfg_path):
    """The 2026-08-18 incident shape, end to end.

    A previously poisoned file carries the legacy key; the deep-merge save
    path preserves unmanaged snapshot keys, so without the purge the legacy
    key would be resurrected into every subsequent save regardless of what
    the collect dict writes.  Saving over a poisoned file must HEAL it.
    """
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    raw.setdefault("risk", {})["max_drawdown_pct"] = 0.1  # the poison
    raw["risk"]["max_drawdown_frac"] = 0.1
    cfg_path.write_text(yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_path))
    assert panel.save_config(str(cfg_path)) is True

    healed = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    assert "max_drawdown_pct" not in healed["risk"], (
        "saving over a poisoned config must remove the fatal key, not "
        "resurrect it from the snapshot"
    )
    assert healed["risk"]["max_drawdown_frac"] == pytest.approx(0.1)


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
