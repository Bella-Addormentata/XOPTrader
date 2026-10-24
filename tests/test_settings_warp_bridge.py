"""[WARP-GUI 2026-10-24] Settings -> Fees & Reserves: the warp bridge controls.

The page now edits the inbound bridge (auto-bridge, its minimum, the per-warp
cap, the USDC kept on Base) and the ETH fee reserve (the warp rebalancer's ETH
band). These tests pin load -> collect -> save, the cases where the page must
NOT write (no warp section; a hand-configured USDC band it does not own), and
the validation that keeps a band the rebalancer would refuse -- which fails
closed and stops every automatic bridge -- from ever being saved.

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


def _cfg(tmp_path, warp):
    dest = tmp_path / "config.yaml"
    shutil.copyfile(_REPO / "config.example.yaml", dest)
    raw = yaml.safe_load(dest.read_text(encoding="utf-8"))
    if warp is None:
        raw.pop("warp", None)
    else:
        raw["warp"] = warp
    dest.write_text(yaml.safe_dump(raw), encoding="utf-8")
    return dest


LIVE = {
    "enabled": True, "dry_run": False, "auto_bridge": True,
    "min_auto_bridge_usdc": 100, "max_auto_bridge_usdc": 5,
    "min_base_eth": 0.005,
}


def _warp(panel) -> dict:
    return panel._collect_config_dict()["warp"]


def test_no_warp_section_writes_only_the_gas_warning(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, None)))
    assert set(_warp(panel)) == {"min_base_eth"}
    assert not panel._warp_auto_bridge.isEnabled()
    assert not panel._eth_auto_topup.isEnabled()


def test_loaded_values_populate_and_round_trip(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, {**LIVE, "keep_usdc_on_base": 20})))
    assert panel._warp_auto_bridge.isChecked()
    assert panel._warp_min_auto.value() == 100
    assert panel._warp_max.value() == 5
    assert panel._warp_keep.value() == 20
    out = _warp(panel)
    assert out["auto_bridge"] is True
    assert out["min_auto_bridge_usdc"] == 100
    assert out["max_auto_bridge_usdc"] == 5
    assert out["keep_usdc_on_base"] == 20
    assert "rebalance" not in out, "no block on disk and top-up off: write none"


def test_unlimited_is_preserved(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, {**LIVE, "max_auto_bridge_usdc": "unlimited"})))
    assert panel._warp_max_unlimited.isChecked()
    assert not panel._warp_max.isEnabled()
    assert _warp(panel)["max_auto_bridge_usdc"] == "unlimited"


def test_a_minimum_above_the_cap_is_flagged(panel, tmp_path):
    """Today's live config: min 100 > max 5, so auto-bridge never fires."""
    panel.load_config(str(_cfg(tmp_path, LIVE)))
    assert not panel._warp_caps_warn.isHidden()
    assert "can never start" in panel._warp_caps_warn.text()
    panel._warp_min_auto.setValue(1)
    assert "can never start" not in panel._warp_caps_warn.text()


def test_ticking_auto_top_up_writes_the_eth_band(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, LIVE)))
    panel._eth_auto_topup.setChecked(True)
    panel._eth_reserve_target.setValue(0.01)
    panel._eth_reserve_tol.setValue(25)
    assert _warp(panel)["rebalance"] == {
        "enabled": True, "eth": {"target": 0.01, "tolerance_pct": 25.0}}


def test_a_saved_band_is_one_the_rebalancer_accepts(panel, tmp_path):
    from gui.services.warp import rebalance as rb

    path = _cfg(tmp_path, LIVE)
    panel.load_config(str(path))
    panel._eth_auto_topup.setChecked(True)
    assert panel.save_config(str(path)) is True
    warp = yaml.safe_load(path.read_text(encoding="utf-8"))["warp"]
    params = rb.parse_rebalance_config(warp, min_gas_wei=300_000_000_000_000)
    assert params.enabled and params.eth is not None and params.usdc is None
    assert warp["dry_run"] is False, "keys the page does not own survive"


def test_unticking_keeps_a_hand_configured_usdc_band(panel, tmp_path):
    usdc = {"target": 100, "tolerance_pct": 20}
    panel.load_config(str(_cfg(tmp_path, {**LIVE, "rebalance": {
        "enabled": True, "usdc": usdc,
        "eth": {"target": 0.01, "tolerance_pct": 20}}})))
    assert panel._eth_auto_topup.isChecked()
    panel._eth_auto_topup.setChecked(False)
    assert _warp(panel)["rebalance"] == {"enabled": True, "eth": None}

    path = tmp_path / "config.yaml"
    assert panel.save_config(str(path)) is True
    reb = yaml.safe_load(path.read_text(encoding="utf-8"))["warp"]["rebalance"]
    assert reb["usdc"] == usdc and reb["enabled"] is True
    assert reb.get("eth") is None


def test_unticking_an_eth_only_band_turns_the_rebalancer_off(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, {**LIVE, "rebalance": {
        "enabled": True, "eth": {"target": 0.01, "tolerance_pct": 20}}})))
    panel._eth_auto_topup.setChecked(False)
    assert _warp(panel)["rebalance"] == {"enabled": False, "eth": None}


@pytest.mark.parametrize("target,tol,why", [
    (0.0004, 50.0, "bottom"),      # bottom 0.0002 ETH <= the 0.0003 floor
    (0.002, 20.0, "half-width"),   # half-width 0.0004 < 0.0006
])
def test_a_band_the_rebalancer_would_refuse_cannot_be_saved(
        panel, tmp_path, target, tol, why):
    panel.load_config(str(_cfg(tmp_path, LIVE)))
    panel._eth_auto_topup.setChecked(True)
    panel._eth_reserve_target.setValue(target)
    panel._eth_reserve_tol.setValue(tol)
    errors = panel._validate()
    assert any("ETH fee reserve" in e and why in e for e in errors), errors


def test_a_band_is_not_validated_while_top_up_is_off(panel, tmp_path):
    panel.load_config(str(_cfg(tmp_path, LIVE)))
    panel._eth_reserve_target.setValue(0.0004)
    assert not any("ETH fee reserve" in e for e in panel._validate())
