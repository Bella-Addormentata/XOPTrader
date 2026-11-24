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

from gui.widgets.options_settings import (  # noqa: E402
    _SIZE_AUTO,
    _SIZE_FIXED,
    _TARGET_CUSTOM,
    TARGET_MODES,
)
from gui.widgets.settings import SettingsWidget  # noqa: E402

ADDRESS = "xch1kxxd76v9kwsxrcxgndn3mylnxjhd7klgdns7083zjl247ffge32sr5rsy2"
#: A wallet of its own: the example config shares the trading wallet.
_DEDICATED = {"share_trading_wallet": False, "host": "localhost", "port": 9356}
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
    options = {
        "enabled": True,
        "wallet": {"share_trading_wallet": True, "address": ADDRESS},
        "contract_xch": 0.5,
        "expiries": ["2026-12-04T14:00:00Z"],
        "call_strikes_pct": [110, 120],
        "put_strikes_pct": [],
        "limits": {"max_locked_xch": 1.0, "max_locked_quote": 1, "max_contracts": 2},
        **extra,
    }
    return _config(tmp_path, secrets={"chia": _SECRETS["chia"]}, **options)


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
    assert tab.size_mode.currentIndex() == _SIZE_FIXED and not tab.contract_xch.isHidden()
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
    assert after["wallet"]["engine_metrics_url"] == before["wallet"]["engine_metrics_url"]
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
    panel.load_config(str(_config(tmp_path, wallet=_DEDICATED)))
    tab = panel._options_section
    assert not tab.share.isChecked() and not tab.fence.isEnabled()
    assert not any(field.isHidden() for _label, field in tab._dedicated_rows)
    assert tab.dedicated_port.value() == 9356
    assert tab.dedicated_fingerprint.text() == "4242", "from secrets.yaml"
    tab.share.setChecked(True)
    assert tab.fence.isEnabled()
    assert all(field.isHidden() for _label, field in tab._dedicated_rows)


# -- review of #246 ------------------------------------------------------------------------


def test_switching_to_the_shared_wallet_clears_the_dedicated_connection(panel, tmp_path):
    cfg_path = _config(
        tmp_path,
        wallet=_DEDICATED,
        enabled=True,
        contract_xch=0.5,
        expiries=["2026-12-04T14:00:00Z"],
    )
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.dedicated_port.value() == 9356 and tab.dedicated_fingerprint.text() == "4242"
    tab.share.setChecked(True)
    tab.address.setText(ADDRESS)
    tab.limit_fields["max_locked_xch"].setText("1")
    assert panel.save_config(str(cfg_path)) is True, _SAID

    wallet = _saved_options(cfg_path)["wallet"]
    assert "port" not in wallet and "host" not in wallet, "the Connection tab's now"
    secrets = yaml.safe_load((tmp_path / "secrets.yaml").read_text(encoding="utf-8"))
    assert not (secrets.get("options") or {}).get("wallet"), "the old key and certificates go"
    for key, value in _SECRETS["chia"].items():
        assert secrets["chia"][key] == value, key


def test_edited_dedicated_secrets_reach_secrets_yaml(panel, tmp_path):
    cfg_path = _config(
        tmp_path,
        wallet=_DEDICATED,
        enabled=True,
        contract_xch=0.5,
        expiries=["2026-12-04T14:00:00Z"],
        limits={"max_locked_xch": 1.0, "max_contracts": 2},
    )
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    tab.dedicated_fingerprint.setText("5151")
    tab.dedicated_cert.setText("/opt/new.crt")
    assert panel.save_config(str(cfg_path)) is True, _SAID
    wallet = yaml.safe_load((tmp_path / "secrets.yaml").read_text(encoding="utf-8"))["options"][
        "wallet"
    ]
    assert wallet["fingerprint"] == 5151 and wallet["cert_path"] == "/opt/new.crt"
    assert wallet["key_path"] == "/opt/w.key", "untouched, as it was"


def test_a_dedicated_wallet_never_saves_a_fence(panel, tmp_path):
    panel.load_config(str(_shared_book(tmp_path)))
    tab = panel._options_section
    tab.fence.setCurrentIndex(1)  # Always on
    tab.share.setChecked(False)
    assert tab.collect()["wallet"]["engine_fence"] == "auto"


def test_a_zero_tolerance_stays_zero(panel, tmp_path):
    panel.load_config(str(_shared_book(tmp_path, target={"xch": 2, "tolerance_pct": 0})))
    assert panel._options_section.target_tolerance.value() == 0


def test_numeric_expiries_show_as_dates(panel, tmp_path):
    panel.load_config(str(_shared_book(tmp_path, expiries=[1796392800])))
    assert panel._options_section.expiries.text() == "2026-12-04T14:00:00Z"


def test_an_infinite_limit_is_refused_not_crashed_on(panel, tmp_path):
    cfg_path = _shared_book(tmp_path)
    panel.load_config(str(cfg_path))
    panel._options_section.limit_fields["max_locked_xch"].setText("inf")
    assert panel.save_config(str(cfg_path)) is False
    assert _SAID and "not a number" in _SAID[0]


def test_a_connection_change_that_breaks_the_book_is_refused(panel, tmp_path):
    cfg_path = _shared_book(tmp_path)
    panel.load_config(str(cfg_path))
    panel._wl_host.setText("10.0.0.5")  # the book can only check an engine on this machine
    assert panel.save_config(str(cfg_path)) is False
    assert _SAID and "this change breaks the options book" in _SAID[0]


def test_a_book_already_broken_does_not_block_other_saves(panel, tmp_path):
    cfg_path = _shared_book(tmp_path)
    root = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    root["options"]["wallet"]["address"] = ""  # refused by the book already
    cfg_path.write_text(yaml.safe_dump(root, sort_keys=False), encoding="utf-8")
    panel.load_config(str(cfg_path))
    panel._wl_port.setValue(19256)
    assert panel.save_config(str(cfg_path)) is True, _SAID


# -- [ROLLING] expiries ----------------------------------------------------------------------


def test_rolling_expiries_show_and_save_as_a_rule(panel, tmp_path):
    cfg_path = _shared_book(tmp_path, expiries="weekly")
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.expiry_mode.currentIndex() == 0
    assert (tab.expiry_weeks.value(), tab.expiry_months.value()) == (2, 0)
    assert tab.expiries.isHidden() and not tab.expiry_weeks.isHidden()

    tab.expiry_months.setValue(1)
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["expiries"] == {"weekly": 2, "monthly": 1}

    panel.load_config(str(cfg_path))
    panel._options_section.expiry_months.setValue(0)
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["expiries"] == "weekly"


def test_fixed_dates_are_still_there_for_whoever_wants_them(panel, tmp_path):
    cfg_path = _shared_book(tmp_path, expiries="weekly")
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    tab.expiry_mode.setCurrentIndex(1)
    tab.expiries.setText("2026-12-04T08:00:00Z")
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["expiries"] == ["2026-12-04T08:00:00Z"]


def test_none_winds_down_and_a_fixed_list_needs_a_date(panel, tmp_path):
    cfg_path = _shared_book(tmp_path, expiries="none")
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.expiry_mode.currentIndex() == 2
    tab.expiry_mode.setCurrentIndex(1)  # Fixed dates, none given
    tab.expiries.setText("")
    assert panel.save_config(str(cfg_path)) is False
    assert _SAID and "at least one date" in _SAID[0]
    tab.expiry_mode.setCurrentIndex(2)
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["expiries"] == "none"


def test_a_config_with_no_expiries_shows_weekly(panel, tmp_path):
    cfg_path = _shared_book(tmp_path, expiries=[])
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.expiry_mode.currentIndex() == 0
    assert (tab.expiry_weeks.value(), tab.expiry_months.value()) == (2, 0)


def _dedicated_book(tmp_path: Path) -> Path:
    return _config(
        tmp_path,
        wallet=_DEDICATED,
        enabled=True,
        contract_xch=0.5,
        expiries=["2026-12-04T14:00:00Z"],
        limits={"max_locked_xch": 1.0, "max_contracts": 2},
    )


def _options_secrets(tmp_path: Path) -> dict:
    secrets = yaml.safe_load((tmp_path / "secrets.yaml").read_text(encoding="utf-8")) or {}
    return (secrets.get("options") or {}).get("wallet") or {}


def test_after_a_save_a_secret_put_back_is_an_edit_too(panel, tmp_path):
    """[review] The tab tells its edits from what was saved, not from the load."""
    cfg_path = _dedicated_book(tmp_path)
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    tab.dedicated_cert.setText("/opt/new.crt")
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _options_secrets(tmp_path)["cert_path"] == "/opt/new.crt"
    assert not tab.wants_writing(), "what was saved is the new baseline"

    tab.dedicated_cert.setText("/opt/w.crt")
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _options_secrets(tmp_path)["cert_path"] == "/opt/w.crt"


def test_shared_and_back_to_dedicated_across_saves_keeps_the_connection(panel, tmp_path):
    cfg_path = _dedicated_book(tmp_path)
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    tab.share.setChecked(True)
    tab.address.setText(ADDRESS)
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _options_secrets(tmp_path) == {}

    tab.share.setChecked(False)  # the same connection, typed back in
    tab.dedicated_port.setValue(9356)
    tab.dedicated_fingerprint.setText("4242")
    tab.dedicated_cert.setText("/opt/w.crt")
    tab.dedicated_key.setText("/opt/w.key")
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _options_secrets(tmp_path) == {
        "cert_path": "/opt/w.crt",
        "key_path": "/opt/w.key",
        "fingerprint": 4242,
    }


def test_the_tab_says_nothing_is_minted_until_all_four_limits_are_set(panel, tmp_path):
    """[review] The book mints nothing while a limit is unset, so the tab says so."""
    panel.load_config(str(_shared_book(tmp_path)))  # max per expiry left unset
    tab = panel._options_section
    assert "all four limits" in tab.limits_hint.text()

    tab.limit_fields["max_contracts_per_expiry"].setText("2")
    assert tab.limits_hint.text() == ""

    tab.limit_fields["max_locked_xch"].setText("0.1")  # a contract is 0.5 XCH
    assert "less than one contract" in tab.limits_hint.text()


def test_a_malformed_value_does_not_stop_the_page_loading(panel, tmp_path):
    """[review] The book's parser says what is wrong with it; the page loads the rest."""
    cfg_path = _config(
        tmp_path,
        enabled=False,
        target={"xch": "oops", "quote": None, "tolerance_pct": float("inf")},
        contract_xch="x",
        expiries=[10**20],
        call_strikes_pct=110,
        limits={"max_locked_xch": float("inf")},
    )

    panel.load_config(str(cfg_path))

    tab = panel._options_section
    assert tab.target_mode.currentIndex() == _TARGET_CUSTOM
    assert tab.target_xch.value() == 0.0 and tab.target_tolerance.value() == 20
    assert tab.contract_xch.value() == 0.0, "malformed: shown as not set"
    assert tab.contract_xch.specialValueText() == "not set"
    assert tab.call_strikes.text() == "110"
    assert tab.expiries.text() == str(10**20)
    assert tab.limit_fields["max_locked_xch"].text() == "inf"
    assert panel._cg_coin_ids.text(), "the tabs after it loaded too"


def test_numbers_the_boxes_cannot_show_are_kept_unless_edited(panel, tmp_path):
    """[review] A box shows a few decimals: an edit elsewhere must not round its number."""
    cfg_path = _shared_book(
        tmp_path,
        contract_xch=0.123456,
        target={"xch": 1.23456, "quote": 2, "tolerance_pct": 12.5},
        limits={
            "max_locked_xch": 1.0,
            "max_locked_quote": 1,
            "max_contracts": 2,
            "max_contracts_per_expiry": 2,
        },
    )
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    tab.limit_fields["max_contracts"].setText("3")  # an edit elsewhere
    assert panel.save_config(str(cfg_path)) is True, _SAID
    saved = _saved_options(cfg_path)
    assert saved["contract_xch"] == 0.123456
    assert saved["target"] == {"xch": 1.23456, "quote": 2, "tolerance_pct": 12.5}

    tab.contract_xch.setValue(0.25)  # edited: the box's value
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["contract_xch"] == 0.25


def test_load_from_editor_writes_the_options_it_imports(panel, tmp_path, monkeypatch):
    """[review] Load from Editor, then Save: the Options tab's imported values are written."""
    monkeypatch.setattr(
        QMessageBox, "question", staticmethod(lambda *a, **k: QMessageBox.StandardButton.Yes)
    )
    cfg_path = _shared_book(tmp_path)
    panel.load_config(str(cfg_path))
    tab = panel._options_section

    panel._on_load_from_editor()  # the editor as loaded: nothing to write
    assert not tab.wants_writing()

    root = yaml.safe_load(panel._yaml_editor.toPlainText())
    root["options"]["expiries"] = "monthly"
    root["options"]["contract_xch"] = 0.25
    panel._yaml_editor.setPlainText(yaml.safe_dump(root, sort_keys=False))
    panel._on_load_from_editor()

    assert tab.wants_writing()
    assert tab.edited_secrets() == frozenset(), "the secrets are told from the files"
    assert panel.save_config(str(cfg_path)) is True, _SAID
    saved = _saved_options(cfg_path)
    assert saved["expiries"] == "monthly" and saved["contract_xch"] == 0.25


def test_a_malformed_contract_size_is_never_replaced_by_an_edit_elsewhere(panel, tmp_path):
    """[review] A size the tab cannot show stays as written, and the book refuses it on
    Save: an edit elsewhere must not mint with a size nobody chose."""
    cfg_path = _shared_book(tmp_path, contract_xch="half")
    before = cfg_path.read_text(encoding="utf-8")
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.contract_xch.value() == 0.0

    tab.limit_fields["max_contracts"].setText("3")  # an edit elsewhere
    assert panel.save_config(str(cfg_path)) is False
    assert any("contract_xch" in said for said in _SAID), _SAID
    assert cfg_path.read_text(encoding="utf-8") == before

    tab.contract_xch.setValue(0.5)
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["contract_xch"] == 0.5


def test_a_config_with_no_strikes_shows_and_saves_both_sides(panel, tmp_path):
    """[DEFAULTS] What the book writes with no strikes written is what the tab shows,
    so a save keeps calls and puts instead of writing none."""
    cfg_path = _shared_book(tmp_path)
    root = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    del root["options"]["call_strikes_pct"], root["options"]["put_strikes_pct"]
    cfg_path.write_text(yaml.safe_dump(root, sort_keys=False), encoding="utf-8")
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.call_strikes.text() == "110, 120" and tab.put_strikes.text() == "90, 80"

    tab.limit_fields["max_contracts_per_expiry"].setText("4")  # an edit elsewhere
    assert panel.save_config(str(cfg_path)) is True, _SAID
    saved = _saved_options(cfg_path)
    assert saved["call_strikes_pct"] == [110, 120] and saved["put_strikes_pct"] == [90, 80]


def test_strike_lists_written_null_stay_blank_and_save_empty(panel, tmp_path):
    """[review] Written null is written: no defaults put in by an edit elsewhere."""
    cfg_path = _shared_book(tmp_path, call_strikes_pct=None, put_strikes_pct=None)
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.call_strikes.text() == "" and tab.put_strikes.text() == ""

    tab.limit_fields["max_contracts_per_expiry"].setText("4")  # an edit elsewhere
    assert panel.save_config(str(cfg_path)) is True, _SAID
    saved = _saved_options(cfg_path)
    assert saved["call_strikes_pct"] == [] and saved["put_strikes_pct"] == []


# -- [LADDER] options.contract_xch: auto ------------------------------------------

_FOUR_SLOTS = {
    "max_locked_xch": 4,
    "max_locked_quote": 1,
    "max_contracts": 4,
    "max_contracts_per_expiry": 2,
}
_NO_ROOM = "Max contracts and Max per expiry leave no room for more calls."


def _auto_book(tmp_path: Path, **extra) -> Path:
    """Two call strikes on the next two Fridays, sized by the book: four calls a size."""
    return _shared_book(
        tmp_path, **{"contract_xch": "auto", "expiries": "weekly", "limits": _FOUR_SLOTS, **extra}
    )


def test_an_auto_size_shows_as_automatic_and_is_saved_as_auto(panel, tmp_path):
    cfg_path = _auto_book(tmp_path)
    panel.load_config(str(cfg_path))
    tab = panel._options_section

    assert tab.size_mode.currentIndex() == _SIZE_AUTO
    assert tab.contract_xch.isHidden() and tab._size_label.isHidden()
    assert not tab.size_hint.isHidden()
    assert tab.size_hint.text() == f"Calls, smallest first: 4 of 1 XCH (4 XCH locked). {_NO_ROOM}"
    assert "5 XCH and 2 of the quote asset" in tab.target_hint.text(), "4 XCH plus one contract"
    assert tab.limits_hint.text() == ""
    assert not panel._dirty, "showing it is not an edit"

    tab.limit_fields["max_locked_quote"].setText("2")  # an edit elsewhere
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["contract_xch"] == "auto"


@pytest.mark.parametrize("written", [None, "AUTO"])
def test_a_size_not_written_shows_as_automatic(panel, tmp_path, written):
    """[LADDER] The book reads no size as auto, so the tab shows that."""
    cfg_path = _auto_book(tmp_path, contract_xch=written)
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.size_mode.currentIndex() == _SIZE_AUTO

    tab.limit_fields["max_locked_quote"].setText("2")
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["contract_xch"] == "auto", "written out once saved"


def test_the_ladder_hint_follows_the_limits_and_the_grid(panel, tmp_path):
    """[LADDER] What the ladder comes to for calls, and what one more call needs."""
    panel.load_config(str(_auto_book(tmp_path)))
    tab = panel._options_section
    cap = tab.limit_fields["max_locked_xch"]

    cap.setText("40")
    assert tab.size_hint.text().endswith(_NO_ROOM), "four contracts: the 1 XCH grid only"
    assert "41 XCH" in tab.target_hint.text(), "the headroom is one 1 XCH contract"

    tab.limit_fields["max_contracts"].setText("8")
    tab.limit_fields["max_contracts_per_expiry"].setText("4")
    assert tab.size_hint.text() == (
        "Calls, smallest first: 4 of 1 XCH, then 3 of 10 XCH (34 XCH locked). "
        "Max XCH locked 44 adds a 10 XCH call."
    )
    cap.setText("44")
    assert tab.size_hint.text() == (
        f"Calls, smallest first: 4 of 1 XCH, then 4 of 10 XCH (44 XCH locked). {_NO_ROOM}"
    )
    tab.limit_fields["max_contracts"].setText("12")
    tab.limit_fields["max_contracts_per_expiry"].setText("6")
    assert tab.size_hint.text().endswith("Max XCH locked 144 adds a 100 XCH call.")

    tab.call_strikes.setText("110, 120, 130")  # more variety: six calls a size
    assert tab.size_hint.text() == (
        "Calls, smallest first: 6 of 1 XCH, then 3 of 10 XCH (36 XCH locked). "
        "Max XCH locked 46 adds a 10 XCH call."
    )
    cap.setText("2")
    assert tab.size_hint.text() == (
        "Calls, smallest first: 2 of 1 XCH (2 XCH locked). Max XCH locked 3 adds a 1 XCH call."
    )

    cap.setText("0.5")
    assert "less than one contract" in tab.limits_hint.text()
    assert tab.size_hint.text() == "", "the limits hint says it"

    for no_number in ("", "unlimited"):
        cap.setText(no_number)
        assert "has no number to size more from" in tab.size_hint.text()

    cap.setText("40")
    tab.put_strikes.setText("90, 80")  # five strikes a Friday, room for six
    assert tab.size_hint.text().endswith(
        "Puts get the same sizes, as far as Max quote locked allows. Where puts and calls "
        "share Max contracts and Max per expiry, the strikes nearest the money go first, so "
        "puts may take some of the room shown for calls."
    )
    tab.call_strikes.setText("")
    assert tab.size_hint.text().startswith("1 XCH contracts first, then 10, 100...")


def test_puts_take_their_share_of_the_counts_in_the_hint(panel, tmp_path):
    """[LADDER] Two puts a Friday at each size: 8 contracts are the 1 XCH grid alone."""
    panel.load_config(str(_auto_book(tmp_path, put_strikes_pct=[90, 80])))
    tab = panel._options_section
    tab.limit_fields["max_locked_xch"].setText("44")
    tab.limit_fields["max_contracts"].setText("8")
    tab.limit_fields["max_contracts_per_expiry"].setText("4")
    assert tab.size_hint.text().startswith(
        f"Calls, smallest first: 4 of 1 XCH (4 XCH locked). {_NO_ROOM}"
    )
    tab.limit_fields["max_contracts"].setText("16")
    tab.limit_fields["max_contracts_per_expiry"].setText("8")
    assert tab.size_hint.text().startswith(
        "Calls, smallest first: 4 of 1 XCH, then 4 of 10 XCH (44 XCH locked)."
    )


def test_puts_with_an_unlimited_cap_leave_room_for_larger_calls(panel, tmp_path):
    """[review #261] Unlimited Max quote locked keeps puts at 1 XCH, as the book does."""
    limits = {
        "max_locked_xch": 44,
        "max_locked_quote": "unlimited",
        "max_contracts": 16,
        "max_contracts_per_expiry": 8,
    }
    panel.load_config(str(_auto_book(tmp_path, put_strikes_pct=[90, 80], limits=limits)))
    tab = panel._options_section
    assert tab.size_hint.text() == (
        "Calls, smallest first: 4 of 1 XCH, then 4 of 10 XCH (44 XCH locked). "
        "Max XCH locked 144 adds a 100 XCH call. "
        "Puts stay at 1 XCH: Max quote locked has no number to size them from."
    )


def test_the_hint_says_when_puts_share_the_counts_with_calls(panel, tmp_path):
    """[review #261] The hint counts the calls first; the book mints the strikes nearest
    the money first, calls or puts, so where the counts cannot hold both it says so."""
    limits = {
        "max_locked_xch": 44,
        "max_locked_quote": 100,
        "max_contracts": 10,
        "max_contracts_per_expiry": 5,
    }
    panel.load_config(str(_auto_book(tmp_path, put_strikes_pct=[90, 80], limits=limits)))
    assert panel._options_section.size_hint.text() == (
        "Calls, smallest first: 4 of 1 XCH, then 2 of 10 XCH (24 XCH locked). "
        "Max contracts and Max per expiry leave no room for more calls. "
        "Puts get the same sizes, as far as Max quote locked allows. "
        "Where puts and calls share Max contracts and Max per expiry, the strikes nearest "
        "the money go first, so puts may take some of the room shown for calls."
    )


def test_the_ladder_hint_counts_a_month_end_that_is_also_a_weekly_once(panel, tmp_path):
    """[review #261] On 2026-11-24 the next two weeklies are Nov 27 and Dec 4, and
    November's month-end is Nov 27 too: two expiries, as the book counts them."""
    limits = {
        **_FOUR_SLOTS,
        "max_locked_xch": 44,
        "max_contracts": 8,
        "max_contracts_per_expiry": 4,
    }
    cfg_path = _auto_book(tmp_path, expiries={"weekly": 2, "monthly": 1}, limits=limits)
    panel.load_config(str(cfg_path))
    tab = panel._options_section

    tab._clock = lambda: 1795536000  # 2026-11-24 16:00Z
    tab._sync_rows()
    assert tab.size_hint.text().startswith(
        "Calls, smallest first: 4 of 1 XCH, then 4 of 10 XCH (44 XCH locked)."
    )
    tab._clock = lambda: 1796112000  # 2026-12-01 08:00Z: Dec 4, Dec 11 and Dec 25
    tab._sync_rows()
    assert tab.size_hint.text().startswith(
        "Calls, smallest first: 6 of 1 XCH, then 2 of 10 XCH (26 XCH locked)."
    )


def test_the_ladder_hint_leaves_out_dates_the_book_no_longer_mints_for(panel, tmp_path):
    """[review #261] Fixed dates already past or inside the stop window (6 h) get no
    new contract, so the hint counts only the ones the book still mints for."""
    dates = ["2026-11-20T08:00:00Z", "2026-11-24T20:00:00Z", "2026-11-27T08:00:00Z"]
    limits = {**_FOUR_SLOTS, "max_locked_xch": 22, "max_contracts": 4}
    cfg_path = _auto_book(tmp_path, expiries=dates, call_strikes_pct=[110], limits=limits)
    panel.load_config(str(cfg_path))
    tab = panel._options_section

    tab._clock = lambda: 1795536000  # 2026-11-24 16:00Z: only Nov 27 is left
    tab._sync_rows()
    assert tab.size_hint.text() == (
        "Calls, smallest first: 1 of 1 XCH, then 1 of 10 XCH (11 XCH locked). " + _NO_ROOM
    )
    tab._clock = lambda: 1795334400  # 2026-11-22 08:00Z: Nov 24 and Nov 27
    tab._sync_rows()
    assert tab.size_hint.text().startswith(
        "Calls, smallest first: 2 of 1 XCH, then 2 of 10 XCH (22 XCH locked)."
    )


def test_switching_to_fixed_writes_the_number_in_the_box(panel, tmp_path):
    cfg_path = _auto_book(tmp_path)
    panel.load_config(str(cfg_path))
    tab = panel._options_section

    tab.size_mode.setCurrentIndex(_SIZE_FIXED)
    assert not tab.contract_xch.isHidden() and tab.size_hint.isHidden()
    assert tab.contract_xch.value() == 1.0, "what Fixed starts from"
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["contract_xch"] == 1.0

    tab.contract_xch.setValue(2.0)
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["contract_xch"] == 2.0


def test_switching_a_written_size_to_automatic_writes_auto(panel, tmp_path):
    cfg_path = _shared_book(tmp_path, limits=_FOUR_SLOTS)  # contract_xch: 0.5
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.size_mode.currentIndex() == _SIZE_FIXED

    tab.size_mode.setCurrentIndex(_SIZE_AUTO)
    assert panel._dirty
    assert tab.size_hint.text().startswith("Calls, smallest first: 2 of 1 XCH"), "one date"
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["contract_xch"] == "auto"

    # And back, untouched: the number it had, exactly.
    tab.size_mode.setCurrentIndex(_SIZE_FIXED)
    assert tab.contract_xch.value() == 1.0, "a save rebased the tab on auto"


@pytest.mark.parametrize(
    "change",
    [
        lambda tab: tab.size_mode.setCurrentIndex(_SIZE_AUTO),
        lambda tab: tab.contract_xch.setValue(2.0),
    ],
    ids=["to automatic", "to another number"],
)
def test_a_dexie_check_sized_to_the_old_size_goes_with_it(panel, tmp_path, change):
    """[LADDER] The live test wrote hedge_size_xch: 0.5 beside contract_xch: 0.5. A new
    size takes the Dexie check back to its default, one contract of the smallest size,
    instead of leaving it at 0.5 unseen; the rest of options.market stays."""
    cfg_path = _shared_book(tmp_path, limits=_FOUR_SLOTS, market={"hedge_size_xch": 0.5})
    panel.load_config(str(cfg_path))
    change(panel._options_section)

    assert panel.save_config(str(cfg_path)) is True, _SAID
    market = _saved_options(cfg_path)["market"]
    assert "hedge_size_xch" not in market
    assert market["max_spread_pct"] == 10, "the rest of the section is kept"


def test_a_dexie_check_chosen_on_its_own_is_kept(panel, tmp_path):
    """[LADDER] Only a check sized to the old contract goes; one set apart from it stays,
    and so does one beside a size that did not change."""
    cfg_path = _shared_book(tmp_path, limits=_FOUR_SLOTS, market={"hedge_size_xch": 5})
    panel.load_config(str(cfg_path))
    panel._options_section.size_mode.setCurrentIndex(_SIZE_AUTO)
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["market"]["hedge_size_xch"] == 5

    cfg_path = _shared_book(tmp_path, limits=_FOUR_SLOTS, market={"hedge_size_xch": 0.5})
    panel.load_config(str(cfg_path))
    panel._options_section.limit_fields["max_contracts"].setText("6")  # the size stays 0.5
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["market"]["hedge_size_xch"] == 0.5


def test_the_ladder_hint_counts_fixed_dates_as_the_book_does(panel, tmp_path):
    """[review #254] The same expiry listed twice, or in two UTC offsets, is one expiry:
    two call strikes and per-expiry room for two are two 1 XCH calls, not four."""
    limits = {**_FOUR_SLOTS, "max_locked_xch": 20}
    cfg_path = _auto_book(
        tmp_path,
        expiries=["2026-12-04T14:00:00Z", "2026-12-04T15:00:00+01:00"],
        limits=limits,
    )
    panel.load_config(str(cfg_path))
    tab = panel._options_section

    assert tab.size_hint.text() == f"Calls, smallest first: 2 of 1 XCH (2 XCH locked). {_NO_ROOM}"
    assert "21 XCH" in tab.target_hint.text(), "20 XCH plus one 1 XCH contract"

    tab.expiries.setText("2026-12-04T14:00:00Z, 2026-12-11T14:00:00Z")  # two expiries
    assert tab.size_hint.text().startswith("Calls, smallest first: 4 of 1 XCH")

    tab.expiries.setText("next friday, next friday")  # the book refuses it on Save
    assert tab.size_hint.text().startswith("Calls, smallest first:"), "the preview still shows"


def test_a_cap_too_large_to_count_does_not_break_the_preview(panel, tmp_path):
    """[review #254] 1e300 in Max XCH locked is finite but not in mojos: the preview
    treats it as no number, and the book refuses it on Save with its reason."""
    cfg_path = _auto_book(tmp_path)
    panel.load_config(str(cfg_path))
    tab = panel._options_section

    tab.limit_fields["max_locked_xch"].setText("1e300")

    assert "has no number to size more from" in tab.size_hint.text()
    assert tab.size_mode.currentIndex() == _SIZE_AUTO
    assert panel.save_config(str(cfg_path)) is False
    assert any("too large" in said for said in _SAID), _SAID


def _without_peg_tolerance(cfg_path: Path) -> Path:
    root = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    root["options"]["market"].pop("peg_tolerance_pct")
    cfg_path.write_text(yaml.safe_dump(root, sort_keys=False), encoding="utf-8")
    return cfg_path


def test_the_off_peg_tolerance_is_shown_and_kept_as_written(panel, tmp_path):
    """[PEG] config.example.yaml writes 3: shown, and written back as it was when the
    tab is saved for something else."""
    cfg_path = _shared_book(tmp_path)
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.peg_tolerance.value() == 3.0
    tab.limit_fields["max_contracts"].setText("3")
    assert panel.save_config(str(cfg_path)) is True, _SAID
    market = _saved_options(cfg_path)["market"]
    assert market["peg_tolerance_pct"] == 3 and isinstance(market["peg_tolerance_pct"], int)
    assert market["max_spread_pct"] == 10, "the rest of the section is kept"


def test_an_edited_off_peg_tolerance_is_saved_and_the_book_reads_it(panel, tmp_path):
    cfg_path = _shared_book(tmp_path)
    panel.load_config(str(cfg_path))
    panel._options_section.peg_tolerance.setValue(1.5)
    assert panel._dirty
    assert panel.save_config(str(cfg_path)) is True, _SAID

    assert _saved_options(cfg_path)["market"]["peg_tolerance_pct"] == 1.5
    from gui.services.config_split import load_merged
    from gui.services.options import config as options_config

    parsed = options_config.parse(load_merged(cfg_path))  # with secrets.yaml, as the book reads
    assert parsed is not None and parsed.market.peg_tolerance_pct == 1.5


def test_an_unwritten_off_peg_tolerance_stays_the_book_s_default(panel, tmp_path):
    """[PEG] Installs from before it existed have no key: the box shows the book's
    default, and a save for something else leaves it unwritten, so a later default
    still applies. Edited, it is written."""
    cfg_path = _without_peg_tolerance(_shared_book(tmp_path))
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.peg_tolerance.value() == 3.0, "the default"
    tab.limit_fields["max_contracts"].setText("3")
    assert panel.save_config(str(cfg_path)) is True, _SAID
    market = _saved_options(cfg_path)["market"]
    assert "peg_tolerance_pct" not in market
    assert market["max_spread_pct"] == 10

    panel.load_config(str(cfg_path))
    panel._options_section.peg_tolerance.setValue(0.0)  # the strict rule, chosen
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert _saved_options(cfg_path)["market"]["peg_tolerance_pct"] == 0.0


def test_a_tolerance_deleted_in_the_editor_goes_from_the_file(panel, tmp_path, monkeypatch):
    """[review #263] Deleted in the YAML editor and loaded from it, the tolerance shows the
    book's default: a save must take it out of config.yaml too, not keep the old value
    unseen under the default the box shows."""
    monkeypatch.setattr(
        QMessageBox, "question", staticmethod(lambda *a, **k: QMessageBox.StandardButton.Yes)
    )
    cfg_path = _shared_book(tmp_path, market={"peg_tolerance_pct": 0})
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.peg_tolerance.value() == 0.0

    root = yaml.safe_load(panel._yaml_editor.toPlainText())
    root["options"]["market"].pop("peg_tolerance_pct")
    panel._yaml_editor.setPlainText(yaml.safe_dump(root, sort_keys=False))
    panel._on_load_from_editor()

    assert tab.peg_tolerance.value() == 3.0, "the default"
    assert tab.wants_writing()
    assert panel.save_config(str(cfg_path)) is True, _SAID
    market = _saved_options(cfg_path)["market"]
    assert "peg_tolerance_pct" not in market
    assert market["max_spread_pct"] == 10


def test_a_blank_tolerance_is_the_default_to_the_book_and_the_tab(panel, tmp_path):
    """[review #263] `peg_tolerance_pct:` left blank (null) is read by the book as left
    out, so the tab's showing it as the default and saving it as left out changes
    nothing the book does."""
    from gui.services.config_split import load_merged
    from gui.services.options import config as options_config

    cfg_path = _shared_book(tmp_path, market={"peg_tolerance_pct": None})
    parsed = options_config.parse(load_merged(cfg_path))
    assert parsed is not None and parsed.market.peg_tolerance_pct == 3.0
    panel.load_config(str(cfg_path))
    tab = panel._options_section
    assert tab.peg_tolerance.value() == 3.0
    tab.limit_fields["max_contracts"].setText("3")
    assert panel.save_config(str(cfg_path)) is True, _SAID
    assert "peg_tolerance_pct" not in _saved_options(cfg_path)["market"]
    parsed = options_config.parse(load_merged(cfg_path))
    assert parsed is not None and parsed.market.peg_tolerance_pct == 3.0
