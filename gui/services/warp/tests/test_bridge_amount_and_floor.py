"""[WARP-GUI 2026-10-24] Typed Bridge now amounts and the keep-on-Base floor.

Two operator controls on the inbound (USDC -> wUSDC.b) path:

* a **typed amount** on Bridge now is bridged EXACTLY: refused up front when
  it is above the per-warp cap or not a whole wUSDC.b mojo, and the job waits
  for the balance to cover it instead of bridging whatever happens to be
  there;
* ``warp.keep_usdc_on_base`` is left behind whenever the bridge sizes a job
  itself -- every automatic bridge (legacy or rebalancer), and Bridge now with
  no amount typed -- but never reduces a typed amount.

And the rebalancer interplay: with an ETH-only band, ``auto_bridge`` used to
stop working outright (the rebalancer owns automatic bridging and had no leg
for it). It now runs as the planner's last leg, after any gas action.
"""

from __future__ import annotations

import pytest

pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")

from gui.services.warp import rebalance as rb  # noqa: E402
from gui.services.warp import service as S  # noqa: E402
from gui.services.warp.jobs import JobStatus  # noqa: E402

from .test_warp_service import build, default_params, new_store, seed  # noqa: E402


# --------------------------------------------------------------------------- #
# Config.
# --------------------------------------------------------------------------- #

def test_keep_on_base_defaults_to_zero_and_scales_to_micros():
    assert S.warp_params_from_config({}).keep_usdc_micros == 0
    assert S.warp_params_from_config(
        {"warp": {"keep_usdc_on_base": 20}}).keep_usdc_micros == 20_000_000
    assert S.warp_params_from_config(
        {"warp": {"keep_usdc_on_base": None}}).keep_usdc_micros == 0


@pytest.mark.parametrize("bad", [-1, "abc", float("nan")])
def test_a_bad_keep_on_base_fails_loudly(bad):
    with pytest.raises(S.WarpError):
        S.warp_params_from_config({"warp": {"keep_usdc_on_base": bad}})


# --------------------------------------------------------------------------- #
# Typed amounts.
# --------------------------------------------------------------------------- #

def test_a_typed_amount_is_marked_exact():
    store = new_store()
    engine, _ = build(store)
    engine.request_bridge(target_micros=2_500_000)
    job = store.get_active_job()
    assert job.state["exact"] is True and job.state["manual"] is True


def test_an_automatic_target_is_not_exact():
    store = new_store()
    engine, _ = build(store)
    engine.request_bridge(target_micros=2_500_000, automatic=True)
    assert "exact" not in store.get_active_job().state


def test_a_typed_amount_above_the_cap_is_refused_not_clamped():
    store = new_store()
    engine, _ = build(store, params=default_params(max_micros=5_000_000))
    with pytest.raises(S.WarpError, match="refusing rather than clamping"):
        engine.request_bridge(target_micros=5_001_000)
    assert store.get_active_job() is None, "no job is created"
    engine.request_bridge(target_micros=5_000_000)
    assert store.get_active_job() is not None


@pytest.mark.parametrize("micros", [1, 999, 2_500_500])
def test_a_typed_amount_must_be_whole_mojos(micros):
    store = new_store()
    engine, _ = build(store)
    with pytest.raises(S.WarpError, match="whole number of wUSDC.b mojos"):
        engine.request_bridge(target_micros=micros)
    assert store.get_active_job() is None


def test_a_typed_amount_waits_for_the_balance_then_bridges_exactly():
    store = new_store()
    engine, ctx = build(store, params=default_params(
        keep_usdc_micros=10_000_000))       # floor does NOT apply to it
    engine.request_bridge(target_micros=3_000_000)
    ctx.evm.erc20 = 2_000_000
    out = engine.step()
    assert out["status"] == JobStatus.AWAITING_DEPOSIT, (
        "a typed 3 USDC with 2 USDC on hand waits; it does not bridge 2")
    ctx.evm.erc20 = 12_000_000
    out = engine.step()
    assert out["status"] == JobStatus.DEPOSIT_SEEN
    assert store.get_active_job().amount_usdc_micros == 3_000_000


# --------------------------------------------------------------------------- #
# The keep-on-Base floor.
# --------------------------------------------------------------------------- #

def test_a_blank_bridge_now_leaves_the_floor_behind():
    store = new_store()
    engine, ctx = build(store, params=default_params(keep_usdc_micros=2_000_000))
    ctx.evm.erc20 = 5_000_000
    engine.request_bridge()
    out = engine.step()
    assert out["status"] == JobStatus.DEPOSIT_SEEN
    assert store.get_active_job().amount_usdc_micros == 3_000_000


def test_a_balance_at_or_under_the_floor_waits_and_says_why():
    store = new_store()
    engine, ctx = build(store, params=default_params(keep_usdc_micros=5_000_000))
    ctx.evm.erc20 = 5_000_000
    engine.request_bridge()
    out = engine.step()
    assert out["status"] == JobStatus.AWAITING_DEPOSIT
    msgs = " ".join(
        str(getattr(e, "message", "") or "")
        for e in store.get_events(store.get_active_job().id))
    assert "kept on Base" in msgs, msgs


def test_automatic_and_rebalance_jobs_respect_the_floor():
    store = new_store()
    engine, ctx = build(store, params=default_params(
        keep_usdc_micros=2_000_000, min_micros=1_000_000))
    ctx.evm.erc20 = 10_000_000
    # A rebalance job targets 9 USDC, but only 8 sits above the floor.
    engine.request_bridge(target_micros=9_000_000, automatic=True)
    out = engine.step()
    assert out["status"] == JobStatus.DEPOSIT_SEEN
    assert store.get_active_job().amount_usdc_micros == 8_000_000


def test_legacy_auto_start_counts_only_what_is_above_the_floor():
    store = new_store()
    params = default_params(enabled=True, auto_bridge=True,
                            min_micros=1_000_000, keep_usdc_micros=5_000_000)
    engine, ctx = build(store, params=params)
    ctx.evm.erc20 = 5_500_000            # only 0.5 above the floor
    assert engine.maybe_start_auto_job() is None
    ctx.evm.erc20 = 6_000_000            # 1.0 above: meets the minimum
    assert engine.maybe_start_auto_job() is not None


def test_the_snapshot_reports_the_floor():
    store = new_store()
    engine, _ = build(store, params=default_params(keep_usdc_micros=7_000_000))
    assert engine.snapshot()["keep_usdc_micros"] == 7_000_000


# --------------------------------------------------------------------------- #
# The rebalancer's auto-bridge leg (ETH-only band).
# --------------------------------------------------------------------------- #

ETH_ONLY = rb.RebalanceParams(
    enabled=True, eth=rb.AssetBand(target=10 ** 16, tolerance_pct=20))


def _plan(**over):
    args = dict(params=ETH_ONLY, eth_wei=10 ** 16, usdc_micros=50_000_000,
                millieth_units=0, max_bridge_micros=0, max_unwrap_micros=0,
                min_bridge_micros=1_000_000, auto_bridge_floor_micros=None)
    args.update(over)
    return rb.plan(**args)


def test_with_auto_bridge_off_an_eth_only_band_bridges_nothing():
    assert _plan() is None


def test_with_auto_bridge_on_the_excess_above_the_floor_is_bridged():
    act = _plan(auto_bridge_floor_micros=20_000_000)
    assert act is not None and act.kind == "bridge_usdc"
    assert act.amount == 30_000_000
    assert "kept on Base" in act.reason


def test_the_auto_bridge_leg_keeps_the_minimum_and_the_cap():
    assert _plan(auto_bridge_floor_micros=49_500_000) is None, "0.5 < 1 USDC min"
    act = _plan(auto_bridge_floor_micros=0, max_bridge_micros=5_000_000)
    assert act.amount == 5_000_000


def test_a_gas_top_up_always_goes_before_the_auto_bridge():
    act = _plan(auto_bridge_floor_micros=0, eth_wei=10 ** 15,
                millieth_units=20)
    assert act.kind == "unwrap_millieth"


def test_a_usdc_band_still_governs_bridging_over_the_auto_leg():
    both = rb.RebalanceParams(
        enabled=True,
        usdc=rb.AssetBand(target=40_000_000, tolerance_pct=10),
        eth=rb.AssetBand(target=10 ** 16, tolerance_pct=20))
    act = _plan(params=both, auto_bridge_floor_micros=0)
    assert act.kind == "bridge_usdc" and act.amount == 10_000_000, (
        "the band's excess over its target, not the auto leg's whole balance")
