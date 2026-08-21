"""The advisory calculator must not resolve data paths from its own location.

`scripts/offer_sizing.py` is loaded BY PATH out of the PyInstaller bundle, so
`__file__` lives under `sys._MEIPASS` -- a per-launch temp directory that holds
the bundled scripts and nothing else. Its checkout-relative defaults therefore
named a `config.yaml` and a database that never existed, and the GUI reported
"Suggested % unavailable: [Errno 2] No such file or directory".

These tests pin the guard that turns that into an explanation, and pin the two
GUI workers actually passing the paths they are given.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent


def _load_sizing():
    """Import scripts/offer_sizing.py the same way the GUI does."""
    path = REPO_ROOT / "scripts" / "offer_sizing.py"
    spec = importlib.util.spec_from_file_location("xop_offer_sizing_test", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules["xop_offer_sizing_test"] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop("xop_offer_sizing_test", None)
        raise
    return module


def test_explicit_paths_pass_through_unchanged():
    sizing = _load_sizing()
    assert sizing._require_explicit("config", "C:/x/config.yaml") == "C:/x/config.yaml"


def test_a_checkout_still_gets_its_default():
    """Not frozen: the CLI keeps working with no arguments."""
    sizing = _load_sizing()
    sizing._FROZEN = False
    assert sizing._require_explicit("config", None) is None


def test_a_bundle_refuses_rather_than_naming_a_temp_directory(monkeypatch):
    """Frozen with no path: explain, instead of failing on a temp path."""
    sizing = _load_sizing()
    monkeypatch.setattr(sizing, "_FROZEN", True)
    with pytest.raises(RuntimeError) as excinfo:
        sizing._require_explicit("config", None)
    message = str(excinfo.value)
    # It must say what to do, not merely that a file is missing.
    assert "explicit path" in message
    assert "bundle" in message


def test_both_entry_points_route_through_the_guard():
    """A future entry point that forgets the guard would regress this."""
    source = (REPO_ROOT / "scripts" / "offer_sizing.py").read_text(encoding="utf-8")
    # Config flows through _load_config; the database through resolved_db.
    assert '_require_explicit("config", path)' in source
    assert source.count('_require_explicit("database", db_path)') == 2


def test_the_gui_workers_pass_their_paths():
    """The wallet and settings workers must forward what they are given."""
    wallet = (REPO_ROOT / "gui" / "widgets" / "wallet_balances.py").read_text(
        encoding="utf-8")
    assert "config_path=self._config_path" in wallet
    assert "db_path=self._db_path" in wallet

    settings = (REPO_ROOT / "gui" / "widgets" / "settings.py").read_text(
        encoding="utf-8")
    assert "db_path=self._db_path" in settings
    # And the widget must actually hand the worker its stored path.
    assert "_SuggestedTargetsWorker(self._config_path," in settings


def test_frozen_detection_does_not_sniff_the_path():
    """A checkout under a directory containing "_MEI" is not a bundle.

    Matching that substring made the documented no-argument CLI refuse to run
    beside a config.yaml and database that were both present.
    """
    sizing = _load_sizing()
    assert sizing._FROZEN == bool(getattr(sys, "frozen", False))
    source = (REPO_ROOT / "scripts" / "offer_sizing.py").read_text(
        encoding="utf-8")
    detection = source.split("_FROZEN =")[1].split(chr(10))[0]
    assert "sys" in detection and "frozen" in detection
    assert "REPO_ROOT" not in detection


def test_the_paths_are_re_pushed_when_settings_saves_a_config():
    """A one-time snapshot would keep reading the previous config file."""
    source = (REPO_ROOT / "gui" / "widgets" / "main_window.py").read_text(
        encoding="utf-8")
    assert "_push_sizing_paths" in source
    # It must run on the save signal, not only at wiring time.
    saved = source.split("config_saved.connect(bridge.update_config_path)")[1]
    assert "_push_sizing_paths" in saved.split("def ")[0], (
        "config_saved does not re-push the sizing paths"
    )
