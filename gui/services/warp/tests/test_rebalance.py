"""Target-band rebalancer: fail-closed parsing, the pure planner, and the
worker wiring (idle-only, cooldown, own-wallet receiver)."""

from __future__ import annotations

import pytest

pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")
pytest.importorskip("PySide6")

from gui.services.warp import rebalance as rb  # noqa: E402
from gui.services.warp import service as S  # noqa: E402

MIN_GAS = S._MIN_GAS_WEI


# --------------------------------------------------------------------------- #
# Parsing: fail-closed, exactly like [WARP-CAP-FAIL-OPEN].
# --------------------------------------------------------------------------- #

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


def test_usdc_deficit_unwraps_toward_target_in_mojos():
    a = _plan(usdc_micros=50_000_000)
    assert a.kind == "unwrap_usdc"
    assert a.amount == 50_000, "50 USDC deficit = 50_000 wUSDC.b mojos"
    capped = _plan(usdc_micros=0, max_unwrap_micros=10_000_000)
    assert capped.amount == 10_000
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

class FakeEngine:
    def __init__(self):
        from types import SimpleNamespace

        self._params = SimpleNamespace(
            enabled=True, dry_run=False,
            max_micros=500_000_000, max_unwrap_micros=500_000_000,
            min_micros=1_000_000,
        )
        self._store = SimpleNamespace(get_active_job=lambda: self.active)
        self.active = None
        self._hot_cache = {
            "eth_wei": 10 ** 16, "usdc_micros": 150_000_000,
            "millieth_units": 0, "error": None,
        }
        self._hot_address = "0x" + "ab" * 20
        self.unmined = 0
        self._evm = SimpleNamespace(
            get_nonce=lambda addr, pending=True: 7 + (
                self.unmined if pending else 0))
        self.calls: list = []

    def request_bridge(self, target_micros=None):
        self.calls.append(("bridge", target_micros))

    def request_unwrap(self, mojos, receiver, external_relay=False):
        self.calls.append(("unwrap", mojos, receiver))


def _worker_with(params):
    w = S._WarpWorker.__new__(S._WarpWorker)
    w._rebalance_params = params
    w._rebalance_error = ""
    w._rebalance_next_ok = 0.0
    w._rebalance_last = ""
    return w


def test_worker_executes_one_action_and_cools_down():
    w = _worker_with(PARAMS)
    eng = FakeEngine()
    w._maybe_rebalance(eng)
    assert eng.calls == [("bridge", 50_000_000)]
    # Immediately again: cooldown suppresses even though still out of band.
    w._maybe_rebalance(eng)
    assert len(eng.calls) == 1
    assert "above band" in w._rebalance_last


def test_worker_stays_quiet_with_an_active_job_or_dry_run():
    w = _worker_with(PARAMS)
    eng = FakeEngine()
    eng.active = object()
    w._maybe_rebalance(eng)
    assert eng.calls == []
    eng2 = FakeEngine()
    eng2._params.dry_run = True
    w._maybe_rebalance(eng2)
    assert eng2.calls == []


def test_worker_stays_quiet_on_unreadable_balances():
    w = _worker_with(PARAMS)
    eng = FakeEngine()
    eng._hot_cache = {"error": "rpc down"}
    w._maybe_rebalance(eng)
    assert eng.calls == []


def test_auto_unwrap_receiver_is_always_the_engines_own_hot_wallet():
    """An edited config must never redirect an automatic unwrap: the
    receiver comes from the engine, not from any parameter."""
    w = _worker_with(PARAMS)
    eng = FakeEngine()
    eng._hot_cache["usdc_micros"] = 10_000_000       # deep deficit
    w._maybe_rebalance(eng)
    assert eng.calls and eng.calls[0][0] == "unwrap"
    assert eng.calls[0][2] == eng._hot_address


def test_a_failing_actuator_banners_and_cools_down_instead_of_raising():
    w = _worker_with(PARAMS)
    eng = FakeEngine()
    def boom(**kw):
        raise RuntimeError("actuator down")
    eng.request_bridge = boom
    w._maybe_rebalance(eng)                          # must not raise
    assert "rebalance failed" in w._rebalance_last
    assert w._rebalance_next_ok > 0, "failure still starts the cooldown"


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
    single active-job slot. Reachable only with a tiny band (0.002 USDC
    target here), which the parser allows."""
    tiny = rb.RebalanceParams(
        enabled=True, usdc=rb.AssetBand(target=2_000, tolerance_pct=20))
    assert _plan(params=tiny, usdc_micros=2_500,
                 min_bridge_micros=0) is None
    a = _plan(params=tiny, usdc_micros=5_000, min_bridge_micros=0)
    assert a is not None and a.amount == 3_000, "a full mojo still bridges"


class FakeWallet:
    def __init__(self):
        self.calls: list = []

    def wrap_eth(self, amount_wei, *, reserve_wei=0):
        self.calls.append(("wrap_eth", amount_wei, reserve_wei))
        return "0x" + "aa" * 32

    def unwrap_millieth(self, amount_units):
        self.calls.append(("unwrap_millieth", amount_units))
        return "0x" + "bb" * 32


def test_worker_drives_the_eth_actuators_with_the_relay_gas_reserve():
    w = _worker_with(PARAMS)
    wallet = FakeWallet()
    w._base_wallet = lambda: wallet
    eng = FakeEngine()
    eng._hot_cache["eth_wei"] = 3 * 10 ** 16       # above the band
    w._maybe_rebalance(eng)
    assert wallet.calls == [("wrap_eth", 2 * 10 ** 16, S._MIN_GAS_WEI)], (
        "the wrap must carry the relay-gas reserve")
    assert eng.calls == []

    w2 = _worker_with(PARAMS)
    wallet2 = FakeWallet()
    w2._base_wallet = lambda: wallet2
    eng2 = FakeEngine()
    eng2._hot_cache["eth_wei"] = 10 ** 15          # below the band
    eng2._hot_cache["millieth_units"] = 100
    w2._maybe_rebalance(eng2)
    assert wallet2.calls == [("unwrap_millieth", 100)]


def test_garbage_hot_cache_banners_and_cools_down_before_planning():
    """An exception BEFORE the plan (unparseable balance) must still start
    the cooldown -- a broken consult may not retry and log every tick."""
    w = _worker_with(PARAMS)
    eng = FakeEngine()
    eng._hot_cache["eth_wei"] = "garbage"
    w._maybe_rebalance(eng)                        # must not raise
    assert "rebalance failed" in w._rebalance_last
    assert w._rebalance_next_ok > 0
    assert eng.calls == []


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


def test_a_usdc_band_suppresses_the_legacy_auto_bridge():
    """With auto_bridge on, engine.step() consults maybe_start_auto_job
    BEFORE the rebalancer -- an untargeted job would bridge the whole
    balance/cap instead of only the excess above target. The worker marks
    the engine while a USDC band owns bridging."""
    from types import SimpleNamespace

    eng = S.WarpEngine.__new__(S.WarpEngine)
    eng._params = SimpleNamespace(
        enabled=True, auto_bridge=True, dry_run=False, min_micros=1)
    eng._net = SimpleNamespace()
    eng._store = SimpleNamespace(get_active_job=lambda: None)
    eng._evm = SimpleNamespace(
        get_erc20_balance=lambda *a, **k: 10 ** 9)
    eng._hot_address = "0x" + "ab" * 20
    eng.auto_bridge_suppressed = True
    assert eng.maybe_start_auto_job() is None,         "suppressed: the band owns bridging"


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


def test_unsettled_balances_skip_the_consult_without_cooldown():
    """The job actuators (bridge/unwrap) never see the wallet's nonce
    guard, so the worker must skip planning while pending != latest --
    silently, with no cooldown: the next tick re-checks."""
    w = _worker_with(PARAMS)
    eng = FakeEngine()
    eng.unmined = 1
    w._maybe_rebalance(eng)
    assert eng.calls == [] and w._rebalance_next_ok == 0.0
    eng.unmined = 0
    w._maybe_rebalance(eng)
    assert eng.calls, "settles -> the same consult acts"


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
