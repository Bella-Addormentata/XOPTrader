"""[RELOAD] + [ALLOCZERO] Config changes must reach the running engine.

Two halves of the same 2026-08-25 lesson: a Save that only touches disk is
an instruction the engine never hears.

* [RELOAD] the bridge's flag file is the GUI->engine nudge to re-read
  config.yaml (pair disables apply live, everything else restart-required).
* [ALLOCZERO] a disabled pair's asset is OMITTED from the applied
  allocation targets while it is hidden from the table -- absence is the
  engine's neutral semantic, an explicit 0.0 would make a later re-enable
  actively sell the asset to zero. The saved percent must neither ride
  along into config.yaml nor be forgotten for the pair's return.
"""

from __future__ import annotations

import pytest

pytest.importorskip("PySide6")

from PySide6.QtWidgets import QApplication  # noqa: E402


@pytest.fixture(scope="module")
def qapp():
    return QApplication.instance() or QApplication([])


# --------------------------------------------------------------------------- #
# [RELOAD] The GUI->engine channel
# --------------------------------------------------------------------------- #

def test_request_config_reload_writes_the_flag(tmp_path):
    from gui.services.engine_bridge import EngineBridge

    bridge = EngineBridge.__new__(EngineBridge)
    bridge._db_path = tmp_path / "db" / "xop.sqlite"
    bridge.request_config_reload()

    flag = tmp_path / "db" / "config_reload.flag"
    assert flag.exists()
    # A second Save before the engine's next heartbeat must not error and
    # must leave exactly one pending reload -- the engine re-reads the
    # whole file, so one nudge carries both saves.
    bridge.request_config_reload()
    assert flag.exists()


# --------------------------------------------------------------------------- #
# [ALLOCZERO] Disabled pairs and allocation targets
# --------------------------------------------------------------------------- #

def _widget(qapp, pairs, allocations):
    from gui.widgets.wallet_balances import WalletBalancesWidget

    w = WalletBalancesWidget()
    w._last_pairs_cfg = pairs
    # Replace wholesale: the ctor may have restored the developer's real
    # QSettings blob, and this test must not depend on it.
    w._target_allocations = dict(allocations)
    w._target_tolerances = {}
    return w


def test_disabled_pair_asset_is_omitted_not_zeroed(qapp):
    w = _widget(
        qapp,
        pairs=[
            {"name": "XCH/wUSDC.b", "enabled": True},
            {"name": "wmilliETH.b/XCH", "enabled": False},
        ],
        allocations={"XCH": 60.0, "WUSDC.B": 10.0, "WMILLIETH.B": 10.0},
    )
    captured = []
    w.allocation_targets_applied.connect(
        lambda a, p, t, b: captured.append(a))
    w._apply_targets()

    assert len(captured) == 1
    applied = captured[0]
    assert "WMILLIETH.B" not in applied, (
        "an asset whose every pair is disabled must be OMITTED -- an "
        "explicit 0.0 would make a later re-enable+restart actively sell "
        "the asset toward zero")
    assert applied["XCH"] == pytest.approx(0.60)
    assert applied["WUSDC.B"] == pytest.approx(0.10)
    # The saved percent survives for the pair's return.
    assert w._target_allocations["WMILLIETH.B"] == 10.0
    assert "omitted WMILLIETH.B" in w._alloc_hint_label.text()
    w.close()


def test_all_pairs_disabled_is_not_treated_as_unknown(qapp):
    # A KNOWN pairs config with zero enabled pairs must omit everything --
    # the kill-everything scenario is exactly when stale targets must not
    # ride along. Only an EMPTY pairs config means "unknown".
    w = _widget(
        qapp,
        pairs=[
            {"name": "XCH/wUSDC.b", "enabled": False},
            {"name": "wmilliETH.b/XCH", "enabled": False},
        ],
        allocations={"XCH": 60.0, "WUSDC.B": 10.0},
    )
    captured = []
    w.allocation_targets_applied.connect(
        lambda a, p, t, b: captured.append(a))
    w._apply_targets()

    assert captured and captured[0] == {}, (
        "all pairs disabled must omit every positive target, not fall "
        "back to the unknown-config behaviour")
    w.close()


def test_reenabled_pair_asset_gets_its_percent_back(qapp):
    w = _widget(
        qapp,
        pairs=[
            {"name": "XCH/wUSDC.b", "enabled": True},
            {"name": "wmilliETH.b/XCH", "enabled": True},
        ],
        allocations={"XCH": 60.0, "WUSDC.B": 10.0, "WMILLIETH.B": 10.0},
    )
    captured = []
    w.allocation_targets_applied.connect(
        lambda a, p, t, b: captured.append(a))
    w._apply_targets()

    assert captured[0]["WMILLIETH.B"] == pytest.approx(0.10), (
        "re-enabling the pair must bring the SAVED percent back")
    w.close()


def test_unknown_pairs_config_zeroes_nothing(qapp):
    # Before pairs config arrives the enabled set is unknown -- zeroing
    # everything would wipe real targets on a slow startup.
    w = _widget(qapp, pairs=[], allocations={"XCH": 60.0, "BYC": 20.0})
    captured = []
    w.allocation_targets_applied.connect(
        lambda a, p, t, b: captured.append(a))
    w._apply_targets()

    assert captured[0]["XCH"] == pytest.approx(0.60)
    assert captured[0]["BYC"] == pytest.approx(0.20)
    w.close()


def test_disabled_pair_gets_no_ratio_target(qapp):
    w = _widget(
        qapp,
        pairs=[
            {"name": "XCH/wUSDC.b", "enabled": True},
            {"name": "wmilliETH.b/XCH", "enabled": False},
        ],
        allocations={"XCH": 60.0, "WUSDC.B": 10.0, "WMILLIETH.B": 10.0},
    )
    w._last_market_data = {
        "XCH/wUSDC.b": {"mid_price": 1.0},
        "wmilliETH.b/XCH": {"mid_price": 1.0},
    }
    targets = w._pair_ratio_targets_from_allocations()
    assert "XCH/wUSDC.b" in targets
    assert "wmilliETH.b/XCH" not in targets, (
        "a disabled pair must not get a ratio target written to config")
    w.close()
