"""A Settings save keeps the options book's secrets out of config.yaml.

The Settings page loads config.yaml with secrets.yaml laid over it, and every
save splits the two again by ``config_split.SECRET_KEYS``. The options book
(gui/services/options) has no page of its own, so its section rides through a
save untouched, except that its certificate paths and its wallet's fingerprint
must go back to secrets.yaml (which is gitignored) and not into config.yaml
(which is tracked).
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

pytest.importorskip("ruamel.yaml")

from PySide6.QtWidgets import QApplication  # noqa: E402

from gui.widgets.settings import SettingsWidget  # noqa: E402

_SECRETS = {
    "options": {
        "wallet": {"cert_path": "/opt/w.crt", "key_path": "/opt/w.key", "fingerprint": 4242},
        "node": {"cert_path": "/opt/n.crt", "key_path": "/opt/n.key", "ca_cert_path": "/opt/ca"},
    }
}


@pytest.fixture(scope="session")
def qapp():
    app = QApplication.instance() or QApplication([])
    yield app


@pytest.fixture
def panel(qapp, monkeypatch):
    monkeypatch.setattr(SettingsWidget, "_refresh_suggested_targets", lambda self: None)
    widget = SettingsWidget()
    yield widget
    widget.stop_background_work()
    widget.deleteLater()


def test_a_settings_save_keeps_the_options_secrets_in_secrets_yaml(panel, tmp_path):
    cfg_path = tmp_path / "config.yaml"
    shutil.copyfile(_REPO / "config.example.yaml", cfg_path)
    (tmp_path / "secrets.yaml").write_text(yaml.safe_dump(_SECRETS), encoding="utf-8")
    shipped = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))["options"]

    panel.load_config(str(cfg_path))
    panel._wl_port.setValue(19256)  # any edit, on another page
    assert panel.save_config(str(cfg_path)) is True

    public = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    secrets = yaml.safe_load((tmp_path / "secrets.yaml").read_text(encoding="utf-8"))
    # The options section went through the save as it was...
    assert public["options"] == shipped
    for block, keys in _SECRETS["options"].items():
        assert not set(keys) & set(public["options"][block]), block
    # ...and its secrets are where they were, intact.
    assert secrets["options"] == _SECRETS["options"]
    assert public["chia"]["wallet_port"] == 19256


def test_the_shipped_examples_put_the_options_secrets_in_the_secrets_file() -> None:
    public = yaml.safe_load((_REPO / "config.example.yaml").read_text(encoding="utf-8"))
    for block, keys in (("wallet", ("cert_path", "key_path", "fingerprint")),
                        ("node", ("cert_path", "key_path", "ca_cert_path"))):
        present = set(keys) & set(public["options"][block])
        assert not present, f"options.{block} in config.example.yaml carries {present}"
