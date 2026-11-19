"""[OPTIONS] The Settings page's Options tab: the options book set up without YAML.

The tab shows ``options:``, writes back only the keys it owns -- and only once
it has been edited -- and lets the options book's own parser judge what it
would save, so it never accepts a setup the book would then refuse.
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

from PySide6.QtWidgets import QApplication, QMessageBox  # noqa: E402

from gui.widgets.options_settings import TARGET_MODES  # noqa: E402
from gui.widgets.settings import SettingsWidget  # noqa: E402

ADDRESS = "xch1kxxd76v9kwsxrcxgndn3mylnxjhd7klgdns7083zjl247ffge32sr5rsy2"
_SECRETS = {
    "chia": {
        "wallet_fingerprint": 481655774,
        "wallet_cert_path": "/w/private_wallet.crt",
        "wallet_key_path": "/w/private_wallet.key",
    },
    "options": {
        "wallet": {"cert_path": "/opt/w.crt", "key_path": "/opt/w.key", "fingerprint": 4242},
    },
}


@pytest.fixture(scope="session")
def qapp():
    app = QApplication.instance() or QApplication([])
    yield app


#: What the page told the operator in a dialog, this test (never a modal: it would hang).
_SAID: list[str] = []


@pytest.fixture(autouse=True)
def no_modal_dialogs(monkeypatch):
    _SAID.clear()

    def record(*args, **kwargs):
        _SAID.append(str(args[2]) if len(args) > 2 else "")
        return QMessageBox.StandardButton.Ok

    for name in ("warning", "critical", "information"):
        monkeypatch.setattr(QMessageBox, name, staticmethod(record))
    yield


@pytest.fixture
def panel(qapp, monkeypatch):
    monkeypatch.setattr(SettingsWidget, "_refresh_suggested_targets", lambda self: None)
    widget = SettingsWidget()
    yield widget
    widget.stop_background_work()
    widget.deleteLater()


def _config(tmp_path: Path, secrets: dict | None = None, **options) -> Path:
    """config.example.yaml with ``options`` laid over its options section."""
    cfg_path = tmp_path / "config.yaml"
    shutil.copyfile(_REPO / "config.example.yaml", cfg_path)
    root = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    for key, value in options.items():
        if isinstance(value, dict) and isinstance(root["options"].get(key), dict):
            root["options"][key].update(value)
        else:
            root["options"][key] = value
    cfg_path.write_text(yaml.safe_dump(root, sort_keys=False), encoding="utf-8")
    (tmp_path / "secrets.yaml").write_text(
        yaml.safe_dump(_SECRETS if secrets is None else secrets), encoding="utf-8"
    )
    return cfg_path


def _shared_book(tmp_path: Path, **extra) -> Path:
    # Sharing: the engine's own key and certificates, from the chia section.
    return _config(
        tmp_path,
        secrets={"chia": _SECRETS["chia"]},
        enabled=True,
        wallet={"share_trading_wallet": True, "address": ADDRESS},
        contract_xch=0.5,
        expiries=["2026-12-04T14:00:00Z"],
        call_strikes_pct=[110, 120],
        put_strikes_pct=[],
        limits={"max_locked_xch": 1.0, "max_locked_quote": 1, "max_contracts": 2},
        **extra,
    )


def _saved_options(cfg_path: Path) -> dict:
    return yaml.safe_load(cfg_path.read_text(encoding="utf-8"))["options"]


def _refusals(monkeypatch=None) -> list[str]:
    return _SAID


def test_the_settings_page_has_an_options_tab(panel):
    assert "Options" in panel._tab_titles.values()


def test_the_tab_shows_the_options_section(panel, tmp_path):
    panel.load_config(str(_shared_book(tmp_path)))
    tab = panel._options_section
    assert tab.enabled.isChecked() and tab.share.isChecked()
    assert tab.address.text() == ADDRESS
    assert tab.fence.currentIndex() == 0, "auto, the default"
    assert tab.contract_xch.value() == 0.5
    assert tab.expiries.text() == "2026-12-04T14:00:00Z"
    assert tab.call_strikes.text() == "110, 120" and tab.put_strikes.text() == ""
    assert tab.limit_fields["max_locked_xch"].text() == "1"
    assert tab.target_mode.currentText() == TARGET_MODES[0]
    assert "1.5 XCH and 2 of the quote asset" in tab.target_hint.text()
    # Sharing: the connection comes from the Connection tab, the fence applies.
    assert tab.fence.isEnabled()
    assert all(field.isHidden() for _label, field in tab._dedicated_rows)
    assert not panel._dirty, "showing it is not an edit"


def test_editing_the_tab_saves_its_keys_and_keeps_the_rest(panel, tmp_path, monkeypatch):
    cfg_path = _shared_book(tmp_path)
    before = _saved_options(cfg_path)
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    tab.expiries.setText("2026-12-04T14:00:00Z, 2026-12-11T14:00:00Z")
    tab.target_mode.setCurrentIndex(1)  # Custom
    tab.target_xch.setValue(2.0)
    tab.target_quote.setValue(3.0)
    assert panel._dirty
    assert panel.save_config(str(cfg_path)) is True, _refusals(monkeypatch)

    after = _saved_options(cfg_path)
    assert after["expiries"] == ["2026-12-04T14:00:00Z", "2026-12-11T14:00:00Z"]
    assert after["target"] == {"xch": 2.0, "quote": 3.0, "tolerance_pct": 20}
    assert after["wallet"]["engine_fence"] == "auto"
    # What the tab does not own is as it was.
    for key in ("quote_asset_id", "quote_asset_decimals", "dry_run", "market", "node"):
        assert after.get(key) == before.get(key), key
    assert after["wallet"]["mode"] == "dedicated"
    # The options secrets went back to secrets.yaml, not into config.yaml.
    assert "cert_path" not in after["wallet"] and "fingerprint" not in after["wallet"]


def test_from_my_limits_takes_a_written_target_out(panel, tmp_path, monkeypatch):
    cfg_path = _shared_book(tmp_path, target={"xch": 9, "quote": 9})
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.target_mode.currentIndex() == 1
    tab.target_mode.setCurrentIndex(0)
    assert panel.save_config(str(cfg_path)) is True, _refusals(monkeypatch)
    assert "target" not in _saved_options(cfg_path), "an absence: the limits decide"

    panel.load_config(str(cfg_path))
    panel._options_section.target_mode.setCurrentIndex(2)  # Off
    assert panel.save_config(str(cfg_path)) is True
    assert _saved_options(cfg_path)["target"] is False


def test_the_book_s_own_parser_judges_the_tab(panel, tmp_path, monkeypatch):
    cfg_path = _shared_book(tmp_path)
    panel.load_config(str(cfg_path))
    panel._options_section.address.setText("")
    said = _refusals(monkeypatch)
    assert panel.save_config(str(cfg_path)) is False
    assert (
        said
        and "Options: options.wallet.share_trading_wallet needs options.wallet.address" in (said[0])
    )
    assert _saved_options(cfg_path)["wallet"]["address"] == ADDRESS, "nothing was written"


def test_a_strike_that_is_not_a_number_is_refused_not_crashed_on(panel, tmp_path, monkeypatch):
    cfg_path = _shared_book(tmp_path)
    panel.load_config(str(cfg_path))
    panel._options_section.call_strikes.setText("110, abc")  # every keystroke collects
    said = _refusals(monkeypatch)
    assert panel.save_config(str(cfg_path)) is False
    assert said and "is not a number" in said[0]


def test_a_dedicated_wallet_shows_its_own_connection(panel, tmp_path):
    panel.load_config(str(_config(tmp_path)))
    tab = panel._options_section
    assert not tab.share.isChecked() and not tab.fence.isEnabled()
    assert not any(field.isHidden() for _label, field in tab._dedicated_rows)
    assert tab.dedicated_port.value() == 9356
    assert tab.dedicated_fingerprint.text() == "4242", "from secrets.yaml"
    tab.share.setChecked(True)
    assert tab.fence.isEnabled()
    assert all(field.isHidden() for _label, field in tab._dedicated_rows)
