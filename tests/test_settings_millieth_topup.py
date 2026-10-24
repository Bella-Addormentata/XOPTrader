"""[MILLIETH-TOPUP 2026-10-24] Settings -> Fees & Reserves: ETH top-up from
wmilliETH.b.

The page now also edits the wmilliETH.b source of the ETH fee reserve: the
engine's per-coin gas reserve (strategy.asset_reserve_units), the warp
rebalancer's wmilliETH.b block, and the per-unwrap cap. These tests pin
load -> collect -> save, what the page must not invent, and that every
saved combination is one the rebalancer's own parser accepts.
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

from gui.widgets import settings as settings_mod  # noqa: E402
from gui.widgets.settings import SettingsWidget  # noqa: E402

WM = "f322a205c034fe28681829fa5a2e483ac421f0952eb1292945c8db06e0a471a6"
LIVE = {"enabled": True, "dry_run": False, "auto_bridge": False,
        "min_auto_bridge_usdc": 100, "max_auto_bridge_usdc": 5}
BAND = {"enabled": True, "eth": {"target": 0.01, "tolerance_pct": 20}}


@pytest.fixture(scope="session")
def qapp():
    app = QApplication.instance() or QApplication([])
    yield app


@pytest.fixture
def panel(qapp, monkeypatch):
    monkeypatch.setattr(
        SettingsWidget, "_refresh_suggested_targets", lambda self: None)
    w = SettingsWidget()
    yield w
    w.deleteLater()


def _cfg(tmp_path, warp, strategy_extra=None):
    dest = tmp_path / "config.yaml"
    shutil.copyfile(_REPO / "config.example.yaml", dest)
    raw = yaml.safe_load(dest.read_text(encoding="utf-8"))
    if warp is None:
        raw.pop("warp", None)
    else:
        raw["warp"] = warp
    if strategy_extra:
        raw.setdefault("strategy", {}).update(strategy_extra)
    dest.write_text(yaml.safe_dump(raw), encoding="utf-8")
    return dest


def _out(panel) -> dict:
    return panel._collect_config_dict()


def test_the_pinned_asset_id_matches_the_bridge_constants():
    from gui.services.warp import constants

    assert settings_mod._WMILLIETH_ASSET_ID == (
        constants.MAINNET.asset("milliETH").expected_asset_id.lower())


def test_nothing_is_invented_on_a_config_that_never_set_these(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, LIVE)))
    out = _out(panel)
    assert "asset_reserve_units" not in out["strategy"]
    assert "max_unwrap_millieth" not in out["warp"], (
        "stating a cap is what first allows an unwrap")
    assert "rebalance" not in out["warp"]


def test_the_source_needs_the_eth_band(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, LIVE)))
    assert not panel._wm_source.isEnabled()
    panel._eth_auto_topup.setChecked(True)
    assert panel._wm_source.isEnabled()
    assert not panel._wm_buy.isEnabled()
    panel._wm_source.setChecked(True)
    assert panel._wm_buy.isEnabled() and not panel._wm_premium.isEnabled()
    panel._wm_buy.setChecked(True)
    assert panel._wm_premium.isEnabled() and panel._wm_max_spend.isEnabled()


def test_ticking_the_source_writes_a_block_the_rebalancer_accepts(panel, tmp_path):
    from gui.services.warp import rebalance as rb

    path = _cfg(tmp_path, LIVE)
    panel.load_config(str(path))
    panel._eth_auto_topup.setChecked(True)
    panel._wm_source.setChecked(True)
    panel._wm_buy.setChecked(True)
    panel._wm_premium.setValue(2.5)
    panel._wm_max_spend.setValue(4.0)
    panel._wm_unwrap_cap.setValue(3.0)
    assert panel.save_config(str(path)) is True
    warp = yaml.safe_load(path.read_text(encoding="utf-8"))["warp"]
    assert warp["rebalance"]["wmillieth"] == {
        "enabled": True, "buy_on_dexie": True, "max_premium_pct": 2.5,
        "max_spend_xch": 4.0}
    assert warp["max_unwrap_millieth"] == 3.0
    params = rb.parse_rebalance_config(warp, min_gas_wei=300_000_000_000_000)
    assert params.wmillieth is not None and params.wmillieth.buy_on_dexie
    assert params.wmillieth.max_spend_xch_mojos == 4 * 10 ** 12


def test_unticking_the_band_switches_the_source_off(panel, tmp_path):
    """A preserved USDC band keeps the block enabled; a source left on with
    no ETH band would make the parser refuse the whole block."""
    from gui.services.warp import rebalance as rb

    path = _cfg(tmp_path, {**LIVE, "max_unwrap_usdc": 50, "rebalance": {
        **BAND, "usdc": {"target": 100, "tolerance_pct": 20},
        "wmillieth": {"enabled": True}}})
    panel.load_config(str(path))
    assert panel._wm_source.isChecked()
    panel._eth_auto_topup.setChecked(False)
    assert panel.save_config(str(path)) is True
    warp = yaml.safe_load(path.read_text(encoding="utf-8"))["warp"]
    assert warp["rebalance"]["wmillieth"]["enabled"] is False
    rb.parse_rebalance_config(warp, min_gas_wei=300_000_000_000_000)


def test_an_unlimited_cap_round_trips(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, {**LIVE,
                                          "max_unwrap_millieth": "unlimited"})))
    assert panel._wm_unwrap_unlimited.isChecked()
    assert not panel._wm_unwrap_cap.isEnabled()
    assert _out(panel)["warp"]["max_unwrap_millieth"] == "unlimited"


def test_an_unedited_cap_is_written_back_verbatim(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, {**LIVE,
                                          "max_unwrap_millieth": 2.0005})))
    assert _out(panel)["warp"]["max_unwrap_millieth"] == 2.0005


def test_the_gas_reserve_shows_the_global_reserve_until_set(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, LIVE, {"min_reserve_units": 2.0})))
    assert panel._wm_gas_reserve.value() == pytest.approx(2.0)
    assert "asset_reserve_units" not in _out(panel)["strategy"]
    panel._wm_gas_reserve.setValue(4.5)
    assert _out(panel)["strategy"]["asset_reserve_units"] == {WM: 4.5}


def test_the_gas_reserve_keeps_other_coins_and_the_disk_spelling(panel, tmp_path):
    other = "ab" * 32
    path = _cfg(tmp_path, LIVE, {"asset_reserve_units": {
        WM.upper(): 3, other: 7}})
    panel.load_config(str(path))
    assert panel._wm_gas_reserve.value() == pytest.approx(3.0)
    panel._wm_gas_reserve.setValue(5.0)
    assert panel.save_config(str(path)) is True
    strat = yaml.safe_load(path.read_text(encoding="utf-8"))["strategy"]
    assert strat["asset_reserve_units"] == {WM.upper(): 5.0, other: 7}


def test_the_gas_reserve_is_editable_without_a_warp_section(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, None)))
    assert panel._wm_gas_reserve.isEnabled()
    panel._wm_gas_reserve.setValue(2.0)
    assert _out(panel)["strategy"]["asset_reserve_units"] == {WM: 2.0}
