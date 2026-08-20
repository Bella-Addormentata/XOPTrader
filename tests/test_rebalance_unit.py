"""Target-band rebalancer: config parsing and the pure planner.

Dependency-light by design -- no PySide6, no chia_rs, not even the
``gui.services`` package: the module under test is loaded straight from its
path, so the parser and planner keep their coverage in headless unit-only
runs. Worker wiring lives in gui/services/warp/tests/test_rebalance.py.
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys

import pytest

# Loaded straight from its path, NOT via ``gui.services.warp``: importing
# that package runs gui/services/__init__.py, which pulls the Qt-bearing
# services, and living under gui/services/warp/tests/ would do the same at
# collection time. rebalance.py depends on nothing but the stdlib, so this
# module keeps parser/planner coverage alive in headless, unit-only runs
# (proven by running it with PySide6/chia_rs/eth_keys blocked).
_PATH = (pathlib.Path(__file__).resolve().parents[1]
         / "gui" / "services" / "warp" / "rebalance.py")
_SPEC = importlib.util.spec_from_file_location("_warp_rebalance_pure", _PATH)
rb = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = rb
_SPEC.loader.exec_module(rb)

#: Mirrors ``gui.services.warp.service._MIN_GAS_WEI`` (0.0003 ETH) without
#: importing the Qt-bearing service module. ``test_rebalance.py`` asserts the
#: two agree, so drift is caught wherever the real deps are installed.
MIN_GAS = 300_000_000_000_000


def _cfg(**rebalance):
    return {"rebalance": rebalance}


def test_absent_or_disabled_block_is_inert():
    assert rb.parse_rebalance_config({}, min_gas_wei=MIN_GAS).enabled is False
    p = rb.parse_rebalance_config(_cfg(enabled=False), min_gas_wei=MIN_GAS)
    assert p.enabled is False


def test_enabled_without_any_asset_refuses():
    with pytest.raises(rb.RebalanceConfigError, match="at least one"):
        rb.parse_rebalance_config(_cfg(enabled=True), min_gas_wei=MIN_GAS)


def test_malformed_bands_refuse():
    for bad in (
        {"usdc": {"target": -5, "tolerance_pct": 20}},
        {"usdc": {"target": 100, "tolerance_pct": 0}},
        {"usdc": {"target": 100, "tolerance_pct": 120}},
        {"usdc": {"target": 100}},
        {"usdc": "100"},
    ):
        with pytest.raises(rb.RebalanceConfigError):
            rb.parse_rebalance_config(
                _cfg(enabled=True, **bad), min_gas_wei=MIN_GAS
            )


def test_eth_band_must_clear_the_relay_gas_floor():
    """The ETH target doubles as the gas reserve; a band whose bottom dips
    below the relay floor would auto-wrap the wallet into a gas strand."""
    with pytest.raises(rb.RebalanceConfigError, match="relay-gas floor"):
        rb.parse_rebalance_config(
            _cfg(enabled=True, eth={"target": 0.0003, "tolerance_pct": 20}),
            min_gas_wei=MIN_GAS,
        )


def test_a_valid_config_scales_to_base_units():
    p = rb.parse_rebalance_config(
        _cfg(
            enabled=True,
            usdc={"target": 100, "tolerance_pct": 20},
            eth={"target": 0.01, "tolerance_pct": 20},
            cooldown_s=120,
        ),
        min_gas_wei=MIN_GAS,
    )
    assert p.enabled and p.cooldown_s == 120
    assert p.usdc.target == 100_000_000 and p.usdc.low == 80_000_000
    assert p.eth.target == 10 ** 16 and p.eth.high == int(1.2 * 10 ** 16)


def test_short_cooldowns_refuse():
    with pytest.raises(rb.RebalanceConfigError, match="60"):
        rb.parse_rebalance_config(
            _cfg(enabled=True, cooldown_s=5,
                 usdc={"target": 100, "tolerance_pct": 20}),
            min_gas_wei=MIN_GAS,
        )


# --------------------------------------------------------------------------- #
# The planner: deadband semantics, priorities, clamps.
# --------------------------------------------------------------------------- #

PARAMS = rb.RebalanceParams(
    enabled=True,
    usdc=rb.AssetBand(target=100_000_000, tolerance_pct=20),   # 100 USDC
    eth=rb.AssetBand(target=10 ** 16, tolerance_pct=20),       # 0.01 ETH
)


def _plan(**kw):
    base = dict(
        params=PARAMS,
        eth_wei=10 ** 16,
        usdc_micros=100_000_000,
        millieth_units=0,
        max_bridge_micros=500_000_000,
        max_unwrap_micros=500_000_000,
        min_bridge_micros=1_000_000,
    )
    base.update(kw)
    return rb.plan(**base)


def test_in_band_plans_nothing():
    assert _plan() is None
    # Anywhere inside the band, including the edges, is quiet.
    assert _plan(usdc_micros=80_000_000, eth_wei=int(1.2 * 10 ** 16)) is None


def test_usdc_excess_bridges_back_to_target_not_band_edge():
    a = _plan(usdc_micros=150_000_000)
    assert a.kind == "bridge_usdc"
    assert a.amount == 50_000_000, "rebalance moves to TARGET, not the edge"


def test_usdc_excess_respects_the_blast_radius_cap_and_dust_floor():
    a = _plan(usdc_micros=900_000_000, max_bridge_micros=200_000_000)
    assert a.kind == "bridge_usdc" and a.amount == 200_000_000
    assert _plan(usdc_micros=121_000_000, min_bridge_micros=50_000_000) is None


def test_usdc_deficit_unwraps_grossed_up_to_actually_land_on_target():
    """The receiver is credited post-tip: the request must be grossed up
    by the immutable 30 bps or every deficit action lands short and the
    'back to target' contract is false."""
    a = _plan(usdc_micros=50_000_000)
    assert a.kind == "unwrap_usdc"
    assert a.amount == 50_150, "ceil(50 USDC / 0.9970) in mojos"
    credited_micros = (a.amount * 1000) * (10_000 - 30) // 10_000
    assert abs(credited_micros - 50_000_000) <= 1000, (
        "post-tip credit reaches the deficit within one mojo")
    capped = _plan(usdc_micros=0, max_unwrap_micros=10_000_000)
    assert capped.amount == 10_000, "the cap bounds the grossed-up burn"
    assert _plan(usdc_micros=79_999_999, max_unwrap_micros=500) is None


def test_eth_excess_wraps_floored_to_granularity():
    a = _plan(eth_wei=3 * 10 ** 16 + 123_456)
    assert a.kind == "wrap_eth"
    assert a.amount == 2 * 10 ** 16, "wraps down to target, granularity-floored"
    assert a.amount % 10 ** 12 == 0


def test_eth_deficit_refuels_from_held_millieth_and_outranks_usdc():
    """Gas safety first: with BOTH bands breached, the ETH refuel wins."""
    a = _plan(eth_wei=10 ** 15, millieth_units=100, usdc_micros=0)
    assert a.kind == "unwrap_millieth"
    # Deficit 0.009 ETH = 9000 units at 1e12 wei/unit, clamped to the 100 held.
    assert a.amount == 100
    # Without milliETH on hand there is no refuel; USDC deficit is next.
    b = _plan(eth_wei=10 ** 15, millieth_units=0, usdc_micros=0)
    assert b.kind == "unwrap_usdc"


def test_disabled_params_plan_nothing():
    assert _plan(params=rb.RebalanceParams()) is None


# --------------------------------------------------------------------------- #
# Worker wiring: idle-only, cooldown, own-wallet receiver.
# --------------------------------------------------------------------------- #

def test_malformed_shapes_and_quoted_booleans_fail_closed():
    """Falsey-but-present blocks, quoted booleans, bad cooldowns and
    sub-unit targets must all raise (-> banner), never silently default."""
    for warp_cfg in ([], "warp", 0):
        with pytest.raises(rb.RebalanceConfigError):
            rb.parse_rebalance_config(warp_cfg, min_gas_wei=MIN_GAS)
    for bad_block in ([], "", 0):
        with pytest.raises(rb.RebalanceConfigError):
            rb.parse_rebalance_config(
                {"rebalance": bad_block}, min_gas_wei=MIN_GAS)
    # YAML-quoted "false" stays FALSE; junk refuses.
    p = rb.parse_rebalance_config(
        _cfg(enabled="false", usdc={"target": 100, "tolerance_pct": 20}),
        min_gas_wei=MIN_GAS)
    assert p.enabled is False
    with pytest.raises(rb.RebalanceConfigError, match="boolean"):
        rb.parse_rebalance_config(_cfg(enabled="junk"), min_gas_wei=MIN_GAS)
    for bad_cd in (0, "ten", None):
        with pytest.raises(rb.RebalanceConfigError):
            rb.parse_rebalance_config(
                _cfg(enabled=True, cooldown_s=bad_cd,
                     usdc={"target": 100, "tolerance_pct": 20}),
                min_gas_wei=MIN_GAS)
    with pytest.raises(rb.RebalanceConfigError, match="below one base unit"):
        rb.parse_rebalance_config(
            _cfg(enabled=True,
                 usdc={"target": 0.0000001, "tolerance_pct": 20}),
            min_gas_wei=MIN_GAS)
    with pytest.raises(rb.RebalanceConfigError, match="finite"):
        rb.parse_rebalance_config(
            _cfg(enabled=True,
                 usdc={"target": float("inf"), "tolerance_pct": 20}),
            min_gas_wei=MIN_GAS)


def test_sub_mojo_bridge_excess_plans_nothing():
    """1-999 micros of excess is below one CAT mojo: a job for it would
    pend forever on 'deposit below one bridgeable mojo', squatting the
    single active-job slot. The parser REJECTS such tiny bands (see
    test_usdc_half_width_must_absorb_one_mojo); RebalanceParams is built
    directly here to keep the planner defensive in depth anyway."""
    tiny = rb.RebalanceParams(
        enabled=True, usdc=rb.AssetBand(target=2_000, tolerance_pct=20))
    assert _plan(params=tiny, usdc_micros=2_500,
                 min_bridge_micros=0) is None
    a = _plan(params=tiny, usdc_micros=5_000, min_bridge_micros=0)
    assert a is not None and a.amount == 3_000, "a full mojo still bridges"


def test_numeric_overflow_and_fractional_cooldowns_fail_closed():
    for bad in ({"target": 1e308, "tolerance_pct": 20},
                {"target": 10 ** 400, "tolerance_pct": 20}):
        with pytest.raises(rb.RebalanceConfigError):
            rb.parse_rebalance_config(
                _cfg(enabled=True, eth=bad), min_gas_wei=MIN_GAS)
    for bad_cd in (60.9, float("inf")):
        with pytest.raises(rb.RebalanceConfigError):
            rb.parse_rebalance_config(
                _cfg(enabled=True, cooldown_s=bad_cd,
                     usdc={"target": 100, "tolerance_pct": 20}),
                min_gas_wei=MIN_GAS)


def test_eth_bands_narrower_than_the_action_gas_refuse():
    """Each wrap/unwrap pays gas from ETH, so a band narrower than the
    action cost would churn: reach target, pay gas, drop below the band,
    unwrap again, forever."""
    with pytest.raises(rb.RebalanceConfigError, match="half-width"):
        rb.parse_rebalance_config(
            _cfg(enabled=True, eth={"target": 0.01, "tolerance_pct": 0.1}),
            min_gas_wei=MIN_GAS)


def test_an_unset_unwrap_cap_skips_the_deficit_instead_of_refuse_looping():
    """max_unwrap_micros = -1 means never configured: request_unwrap would
    refuse, so planning it would refuse-and-retry every cooldown forever.
    0 stays explicitly unlimited."""
    assert _plan(usdc_micros=50_000_000, max_unwrap_micros=-1) is None
    a = _plan(usdc_micros=50_000_000, max_unwrap_micros=0)
    assert a is not None and a.kind == "unwrap_usdc"


def test_boolean_band_values_refuse():
    """YAML true/false must not read as 1/0: usdc: {target: true, ...}
    would pass as a 1-USDC target and bridge nearly the whole wallet."""
    with pytest.raises(rb.RebalanceConfigError, match="not booleans"):
        rb.parse_rebalance_config(
            _cfg(enabled=True,
                 usdc={"target": True, "tolerance_pct": True}),
            min_gas_wei=MIN_GAS)


def test_usdc_bands_tighter_than_the_tip_refuse():
    """The unwrap receiver gets the post-tip amount, so a 0.1% band can
    never be landed inside by a to-target action -- it would loop."""
    with pytest.raises(rb.RebalanceConfigError, match="tip"):
        rb.parse_rebalance_config(
            _cfg(enabled=True, usdc={"target": 100, "tolerance_pct": 0.1}),
            min_gas_wei=MIN_GAS)


def test_usdc_half_width_must_absorb_one_mojo():
    """target 0.002 / tol 20% gives a 400-micro half-width: breachable by
    a sub-mojo amount no action can move -- permanently breached, silently
    planning nothing. Refused at parse; a 1%-of-100-USDC band passes."""
    with pytest.raises(rb.RebalanceConfigError, match="one"):
        rb.parse_rebalance_config(
            _cfg(enabled=True, usdc={"target": 0.002, "tolerance_pct": 20}),
            min_gas_wei=MIN_GAS)
    p = rb.parse_rebalance_config(
        _cfg(enabled=True, usdc={"target": 100, "tolerance_pct": 1}),
        min_gas_wei=MIN_GAS)
    assert p.usdc.target == 100_000_000


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
