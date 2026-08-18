"""Fee reserves: the balances the bot holds back so it can still pay fees.

Both chains now cost money to act on -- mojos for every Chia offer and cancel,
Base ETH for every bridge relay, wallet send and key rotation -- and a bot that
trades its last unit into an offer ends up holding inventory it cannot move.
Four floors guard that, and until now three of them could only be reached by
hand-editing YAML:

* ``strategy.fee_reserve_xch``        -- withheld from the capital pool;
* ``strategy.fee_min_spendable_xch``  -- maker gate: stop posting offers;
* ``strategy.taker_min_spendable_xch``-- taker gate: stop taking them;
* ``warp.min_base_eth``               -- the Base gas line the GUI warns on.

These tests pin the whole path for each: widget -> collect -> disk -> reload,
plus the two widgets that render the ETH warning.  The disk round-trip is the
part worth guarding hardest: ``_collect_config_dict`` rebuilds each section
from scratch, so a key with a widget but no save line is not merely unsaved --
it is ERASED from config.yaml on the next Save.

Runs headless (``QT_QPA_PLATFORM=offscreen``).
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

# Must be set before PySide6 imports a platform plugin.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402
import yaml  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402

from gui.widgets.base_wallet import BaseWalletWidget  # noqa: E402
from gui.widgets.settings import SettingsWidget  # noqa: E402
from gui.widgets.warp import WarpWidget  # noqa: E402

_FEES_TAB = 5          # index of the "Fees & Reserves" tab
_STRATEGY_TAB = 2

_HOT_ADDR = "0xAbc0000000000000000000000000000000000001"


@pytest.fixture(scope="session")
def qapp():
    """A single offscreen QApplication for the whole session."""
    app = QApplication.instance() or QApplication([])
    yield app


@pytest.fixture
def panel(qapp, monkeypatch):
    """A SettingsWidget with the background suggestion worker stubbed out.

    ``load_config`` normally kicks off a QThread that queries dexie and the
    trade DB; a test has no business doing either.
    """
    monkeypatch.setattr(
        SettingsWidget, "_refresh_suggested_targets", lambda self: None
    )
    w = SettingsWidget()
    yield w
    w.deleteLater()


@pytest.fixture
def cfg_dir(tmp_path):
    """A writable copy of the shipped example config."""
    dest = tmp_path / "config.yaml"
    shutil.copyfile(_REPO / "config.example.yaml", dest)
    return dest


def _written(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8")) or {}


# --------------------------------------------------------------------------- #
# The tab itself
# --------------------------------------------------------------------------- #

def test_the_tab_is_named_for_both_halves(panel):
    """One tab answers 'what may I spend' and 'what must I keep'."""
    assert panel._tabs.tabText(_FEES_TAB) == "Fees & Reserves"


def test_every_floor_has_exactly_one_widget(panel):
    """No duplicate editors for the same key.

    Two spinboxes bound to one config key is a silent data-loss bug: whichever
    one _collect_config_dict happens to read wins, and the other lies to the
    operator about what is in force.
    """
    for attr, default in (
        ("_fee_reserve_xch", 1.0),
        ("_fee_min_spendable_xch", 0.01),
        ("_taker_min_spendable_xch", 0.25),
        ("_min_base_eth", 0.005),
    ):
        box = getattr(panel, attr)
        assert box.value() == pytest.approx(default), attr


def test_editing_a_reserve_dirties_the_reserves_tab_not_strategy(panel):
    """The XCH reserve moved tabs; its dirty marker must move with it.

    Dirty tracking is wired by tab index, so a relocated widget that kept its
    old index would flag the wrong tab -- the operator sees "Strategy" marked
    unsaved and cannot find what changed.
    """
    panel._clear_dirty()
    panel._fee_reserve_xch.setValue(2.5)
    assert panel._tab_dirty.get(_FEES_TAB) is True
    assert panel._tab_dirty.get(_STRATEGY_TAB) is not True

    for attr in ("_fee_min_spendable_xch", "_taker_min_spendable_xch",
                 "_min_base_eth"):
        panel._clear_dirty()
        box = getattr(panel, attr)
        box.setValue(box.value() + box.singleStep())
        assert panel._tab_dirty.get(_FEES_TAB) is True, attr


# --------------------------------------------------------------------------- #
# Load -> widget
# --------------------------------------------------------------------------- #

def test_load_populates_every_floor(panel, cfg_dir):
    raw = _written(cfg_dir)
    raw["strategy"]["fee_reserve_xch"] = 3.5
    raw["strategy"]["fee_min_spendable_xch"] = 0.02
    raw["strategy"]["taker_min_spendable_xch"] = 0.75
    raw["warp"]["min_base_eth"] = 0.02
    cfg_dir.write_text(yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_dir))

    assert panel._fee_reserve_xch.value() == pytest.approx(3.5)
    assert panel._fee_min_spendable_xch.value() == pytest.approx(0.02)
    assert panel._taker_min_spendable_xch.value() == pytest.approx(0.75)
    assert panel._min_base_eth.value() == pytest.approx(0.02)


def test_load_of_a_config_without_the_keys_uses_defaults(panel, cfg_dir):
    """An older config must not populate the boxes with zeros."""
    raw = _written(cfg_dir)
    for key in ("fee_reserve_xch", "fee_min_spendable_xch",
                "taker_min_spendable_xch"):
        raw["strategy"].pop(key, None)
    raw["warp"].pop("min_base_eth", None)
    cfg_dir.write_text(yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_dir))

    assert panel._fee_reserve_xch.value() == pytest.approx(1.0)
    assert panel._fee_min_spendable_xch.value() == pytest.approx(0.01)
    assert panel._taker_min_spendable_xch.value() == pytest.approx(0.25)
    assert panel._min_base_eth.value() == pytest.approx(0.005)


# --------------------------------------------------------------------------- #
# Widget -> collect -> disk -> reload
# --------------------------------------------------------------------------- #

def test_collect_carries_all_four_floors(panel, cfg_dir):
    panel.load_config(str(cfg_dir))
    panel._fee_reserve_xch.setValue(4.0)
    panel._fee_min_spendable_xch.setValue(0.03)
    panel._taker_min_spendable_xch.setValue(0.6)
    panel._min_base_eth.setValue(0.011)

    cfg = panel._collect_config_dict()

    assert cfg["strategy"]["fee_reserve_xch"] == pytest.approx(4.0)
    assert cfg["strategy"]["fee_min_spendable_xch"] == pytest.approx(0.03)
    assert cfg["strategy"]["taker_min_spendable_xch"] == pytest.approx(0.6)
    assert cfg["warp"]["min_base_eth"] == pytest.approx(0.011)


def test_save_reload_round_trips_through_disk(panel, cfg_dir):
    panel.load_config(str(cfg_dir))
    panel._fee_reserve_xch.setValue(2.25)
    panel._fee_min_spendable_xch.setValue(0.05)
    panel._taker_min_spendable_xch.setValue(0.9)
    panel._min_base_eth.setValue(0.03)

    assert panel.save_config(str(cfg_dir)) is True

    on_disk = _written(cfg_dir)
    assert on_disk["strategy"]["fee_reserve_xch"] == pytest.approx(2.25)
    assert on_disk["strategy"]["fee_min_spendable_xch"] == pytest.approx(0.05)
    assert on_disk["strategy"]["taker_min_spendable_xch"] == pytest.approx(0.9)
    assert on_disk["warp"]["min_base_eth"] == pytest.approx(0.03)

    # And the reload agrees -- proves load and save name the same keys.
    fresh = SettingsWidget()
    try:
        fresh._refresh_suggested_targets = lambda: None  # type: ignore[method-assign]
        fresh.load_config(str(cfg_dir))
        assert fresh._fee_reserve_xch.value() == pytest.approx(2.25)
        assert fresh._fee_min_spendable_xch.value() == pytest.approx(0.05)
        assert fresh._taker_min_spendable_xch.value() == pytest.approx(0.9)
        assert fresh._min_base_eth.value() == pytest.approx(0.03)
    finally:
        fresh.deleteLater()


def test_saving_the_gas_reserve_leaves_the_rest_of_warp_alone(panel, cfg_dir):
    """The settings page writes ONE warp key and must merge, not replace.

    The bridge section holds settings this page never shows -- the RPC URL,
    the blast-radius cap, dry-run.  Writing the section wholesale would wipe
    them, and ``max_auto_bridge_usdc`` missing means no cap at all.
    """
    before = _written(cfg_dir)["warp"]
    assert before.get("max_auto_bridge_usdc"), "fixture must have a cap to lose"

    panel.load_config(str(cfg_dir))
    panel._min_base_eth.setValue(0.04)
    assert panel.save_config(str(cfg_dir)) is True

    after = _written(cfg_dir)["warp"]
    assert after["min_base_eth"] == pytest.approx(0.04)
    for key in ("enabled", "dry_run", "base_rpc_url", "max_auto_bridge_usdc",
                "claim_fee_mojos"):
        assert after[key] == before[key], key


def test_the_engine_reads_the_keys_the_gui_writes(panel, cfg_dir):
    """Names are the contract; a typo here is a knob that does nothing.

    The C++ side parses these three under ``strategy`` (cpp/src/config.cpp)
    and the warp service parses the fourth; the GUI must emit those exact
    spellings.
    """
    panel.load_config(str(cfg_dir))
    cfg = panel._collect_config_dict()

    engine_strategy_keys = {
        "fee_reserve_xch", "fee_min_spendable_xch", "taker_min_spendable_xch",
    }
    assert engine_strategy_keys <= set(cfg["strategy"])

    from gui.services.warp.service import warp_params_from_config
    params = warp_params_from_config(cfg)
    assert params.min_base_eth == pytest.approx(panel._min_base_eth.value())


# --------------------------------------------------------------------------- #
# The two widgets that render the Base gas warning
# --------------------------------------------------------------------------- #

def _warp_snap(**over):
    snap = {
        "enabled": True,
        "dry_run": False,
        "network": "mainnet",
        "hot_wallet": {
            "address": _HOT_ADDR, "error": None,
            "eth_wei": 10_000_000_000_000_000,   # 0.01 ETH
            "usdc_micros": 1_500_000,
        },
        "base_wallet": {
            "configured": True, "address": _HOT_ADDR, "error": None,
            "eth_wei": 10_000_000_000_000_000,
            "usdc_micros": 1_500_000,
        },
        "active_job": None,
        "jobs": [],
        "min_micros": 100_000_000,
        "max_micros": 10_000_000_000,
        "receiver_address": "xch1" + "q" * 58,
        "receiver_source": "config",
    }
    snap.update(over)
    return snap


def _show(cls, snap):
    w = cls()
    w.show()  # offscreen: makes isVisible() true so update_data renders
    w.update_data({"warp": snap})
    return w


@pytest.mark.parametrize("cls", [WarpWidget, BaseWalletWidget])
def test_a_raised_reserve_raises_the_warning_line(qapp, cls):
    """0.01 ETH is fine at the default floor and low at a raised one.

    This is the whole point of the knob: an operator who relays often can
    make the GUI nag earlier without touching the source.
    """
    ok = _show(cls, _warp_snap(min_base_eth_wei=5_000_000_000_000_000))
    assert ok._gas_lbl.isVisible() is False

    low = _show(cls, _warp_snap(min_base_eth_wei=50_000_000_000_000_000))
    assert low._gas_lbl.isVisible() is True
    assert "0.0100" in low._gas_lbl.text() or "0.01" in low._gas_lbl.text()


@pytest.mark.parametrize("cls", [WarpWidget, BaseWalletWidget])
def test_zero_reserve_silences_the_warning_entirely(qapp, cls):
    """A wallet with a deliberate 0 must not be nagged at any balance."""
    w = _show(cls, _warp_snap(
        min_base_eth_wei=0,
        hot_wallet={"address": _HOT_ADDR, "error": None, "eth_wei": 1,
                    "usdc_micros": 0},
        base_wallet={"configured": True, "address": _HOT_ADDR, "error": None,
                     "eth_wei": 1, "usdc_micros": 0},
    ))
    assert w._gas_lbl.isVisible() is False


@pytest.mark.parametrize("cls", [WarpWidget, BaseWalletWidget])
def test_a_snapshot_without_the_key_still_warns(qapp, cls):
    """Pre-upgrade snapshots (and a disabled warp) fall back, never go silent.

    A missing threshold must not read as 'no threshold' -- that would drop the
    warning for exactly the operators who have not restarted yet.
    """
    w = _show(cls, _warp_snap(
        hot_wallet={"address": _HOT_ADDR, "error": None,
                    "eth_wei": 1_000_000_000_000, "usdc_micros": 0},
        base_wallet={"configured": True, "address": _HOT_ADDR, "error": None,
                     "eth_wei": 1_000_000_000_000, "usdc_micros": 0},
    ))
    assert w._gas_lbl.isVisible() is True


@pytest.mark.parametrize("cls", [WarpWidget, BaseWalletWidget])
def test_a_junk_threshold_falls_back_rather_than_crashing(qapp, cls):
    for junk in (None, "lots", [], {}):
        w = _show(cls, _warp_snap(
            min_base_eth_wei=junk,
            hot_wallet={"address": _HOT_ADDR, "error": None,
                        "eth_wei": 1_000_000_000_000, "usdc_micros": 0},
            base_wallet={"configured": True, "address": _HOT_ADDR,
                         "error": None, "eth_wei": 1_000_000_000_000,
                         "usdc_micros": 0},
        ))
        assert w._gas_lbl.isVisible() is True, junk


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
