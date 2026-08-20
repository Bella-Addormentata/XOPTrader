"""Target-band rebalancer: worker wiring.

Tick ordering (settle check -> single refresh -> consult), the idle-only and
cooldown rails, actuator dispatch and job provenance. The pure parser and
planner tests live in ``test_rebalance_unit.py``, which needs no Qt.
"""

from __future__ import annotations

import pytest

pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")
pytest.importorskip("PySide6")

from gui.services.warp import rebalance as rb  # noqa: E402
from gui.services.warp import service as S  # noqa: E402

#: Same band as the unit module's, built from the service-side module so
#: these tests never depend on that module's class identity.
PARAMS = rb.RebalanceParams(
    enabled=True,
    usdc=rb.AssetBand(target=100_000_000, tolerance_pct=20),   # 100 USDC
    eth=rb.AssetBand(target=10 ** 16, tolerance_pct=20),       # 0.01 ETH
)


def test_the_unit_modules_gas_floor_mirror_has_not_drifted():
    """tests/test_rebalance_unit.py hardcodes this floor to stay Qt-free
    (it cannot import the service module); if the constant ever changes,
    this fails here, where the deps exist, and names both places."""
    assert S._MIN_GAS_WEI == 300_000_000_000_000, (
        "update the MIN_GAS mirror in tests/test_rebalance_unit.py too")


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

    def refresh_hot_wallet(self):
        self.refreshes = getattr(self, "refreshes", 0) + 1
        fresh = getattr(self, "fresh_cache", None)
        if fresh is not None:
            self._hot_cache = fresh
        return self._hot_cache

    def request_bridge(self, target_micros=None, *, automatic=False):
        self.calls.append(("bridge", target_micros, automatic))

    def request_unwrap(self, mojos, receiver, external_relay=False,
                       *, automatic=False):
        self.calls.append(("unwrap", mojos, receiver, automatic))


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
    assert eng.calls == [("bridge", 50_000_000, True)]
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
    from types import SimpleNamespace

    w = _worker_with(PARAMS)
    eng = FakeEngine()
    eng._hot_cache["usdc_micros"] = 10_000_000       # deep deficit
    eng._resolve_cat_wallet_id = lambda: 7
    eng._wallet = SimpleNamespace(
        get_wallet_balance=lambda wid: {"spendable_balance": 10 ** 9})
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


def test_auto_unwrap_clamps_to_spendable_and_skips_when_empty():
    """A FAILED job retains the single active slot; an automatic unwrap
    sized above held wUSDC.b must clamp (or skip at zero), never open a
    job destined to fail."""
    from types import SimpleNamespace

    w = _worker_with(PARAMS)
    eng = FakeEngine()
    eng._hot_cache["usdc_micros"] = 10_000_000     # 90-USDC deficit
    eng._resolve_cat_wallet_id = lambda: 7
    eng._wallet = SimpleNamespace(
        get_wallet_balance=lambda wid: {"spendable_balance": 40_000})
    w._maybe_rebalance(eng)
    assert eng.calls == [("unwrap", 40_000, eng._hot_address, True)], (
        "clamped to spendable, not the full 90_000-mojo deficit")

    w2 = _worker_with(PARAMS)
    eng2 = FakeEngine()
    eng2._hot_cache["usdc_micros"] = 10_000_000
    eng2._resolve_cat_wallet_id = lambda: 7
    eng2._wallet = SimpleNamespace(
        get_wallet_balance=lambda wid: {"spendable_balance": 0})
    w2._maybe_rebalance(eng2)
    assert eng2.calls == []
    assert "no spendable" in w2._rebalance_last
    assert w2._rebalance_next_ok == 0.0, (
        "a nothing-to-do skip must not arm the cooldown and delay recovery "
        "once inventory becomes spendable")


def test_malformed_warp_block_still_emits_the_banner_snapshot(tmp_path):
    """A warp: list used to raise inside _config_enabled() on the snapshot
    path, so the very banner reporting the malformation was never emitted."""
    worker = S._WarpWorker(secrets_path=tmp_path / "secrets.yaml")
    snaps: list = []
    worker.snapshot_ready.connect(snaps.append)
    worker.set_config({"warp": ["not", "a", "mapping"]})
    worker.tick()
    assert snaps, "the snapshot must still be emitted"
    reb = snaps[-1].get("rebalance") or {}
    assert reb.get("error"), "the parse failure must reach the banner"


def test_tick_checks_settlement_before_its_single_balance_refresh():
    """Ordering rail: the nonce reads must precede the ONE refresh, and
    planning must consume that refresh. A tx mining between an earlier
    refresh and a later nonce check would make the nonces agree while the
    cache was still pre-mine. Also pins one refresh per tick (the snapshot
    and the planner share it) -- no doubled balance RPC."""
    from types import SimpleNamespace

    w = _worker_with(PARAMS)
    eng = FakeEngine()
    order: list = []
    eng._evm = SimpleNamespace(
        get_nonce=lambda addr, pending=True: (
            order.append("nonce:pending" if pending else "nonce:latest") or 7))
    eng._hot_cache = {"eth_wei": 10 ** 16, "usdc_micros": 150_000_000,
                      "millieth_units": 0, "error": None}    # stale pre-mine

    def refresh():
        order.append("refresh")
        eng._hot_cache = {"eth_wei": 10 ** 16, "usdc_micros": 100_000_000,
                          "millieth_units": 0, "error": None}   # in band
        return eng._hot_cache

    eng.refresh_hot_wallet = refresh
    w._rebalance_tick(eng)
    assert order == ["nonce:latest", "nonce:pending", "refresh"]
    assert eng.calls == [], (
        "planning must use the post-refresh in-band balances, not the "
        "stale pre-mine excess")


def test_a_disabled_rebalancer_costs_no_extra_rpc():
    """The snapshot still gets its refresh, but a disabled (or banner-ed)
    rebalancer must not add the settlement reads."""
    from types import SimpleNamespace

    order: list = []
    for params, error in ((rb.RebalanceParams(), ""), (PARAMS, "bad config")):
        w = _worker_with(params)
        w._rebalance_error = error
        eng = FakeEngine()
        order.clear()
        eng._evm = SimpleNamespace(
            get_nonce=lambda addr, pending=True: order.append("nonce") or 7)
        eng.refresh_hot_wallet = lambda: order.append("refresh")
        w._rebalance_tick(eng)
        assert order == ["refresh"], f"{params.enabled}/{error}: {order}"
        assert eng.calls == []


def test_clamped_unwrap_banners_the_actual_amount():
    from types import SimpleNamespace

    w = _worker_with(PARAMS)
    eng = FakeEngine()
    eng._hot_cache["usdc_micros"] = 10_000_000
    eng._resolve_cat_wallet_id = lambda: 7
    eng._wallet = SimpleNamespace(
        get_wallet_balance=lambda wid: {"spendable_balance": 40_000})
    w._maybe_rebalance(eng)
    assert "clamped to 40000 mojos" in w._rebalance_last


def test_usdc_jobs_are_skipped_while_eth_is_below_the_gas_floor():
    """A gasless wallet with no milliETH to refuel from must NOT open a
    USDC job: both job types pend on their gas gates and would squat the
    single active slot. Skipped without cooldown so a refuel acts
    promptly."""
    from types import SimpleNamespace

    w = _worker_with(PARAMS)
    eng = FakeEngine()
    eng._hot_cache.update(
        {"eth_wei": S._MIN_GAS_WEI - 1, "usdc_micros": 10_000_000,
         "millieth_units": 0})
    eng._resolve_cat_wallet_id = lambda: 7
    eng._wallet = SimpleNamespace(
        get_wallet_balance=lambda wid: {"spendable_balance": 10 ** 9})
    w._maybe_rebalance(eng)
    assert eng.calls == []
    assert "relay-gas floor" in w._rebalance_last
    assert w._rebalance_next_ok == 0.0, "no cooldown: a refuel acts promptly"


def test_automatic_jobs_carry_rebalance_provenance_not_manual():
    """manual: True would mis-record the audit trail AND bypass the
    _h_awaiting_deposit auto min_micros recheck -- a rebalance bridge
    could move a sub-floor amount if the balance dropped post-consult."""
    from types import SimpleNamespace

    class PermissiveJob:
        """Any attribute _job_dict asks for reads as None."""

        def __init__(self, state):
            self.state = state
            self.status = "AWAITING_DEPOSIT"

        def __getattr__(self, name):
            return None

    created = {}
    eng = S.WarpEngine.__new__(S.WarpEngine)
    eng._net = SimpleNamespace(name="mainnet")
    eng._store = SimpleNamespace(
        get_active_job=lambda: None,
        create_job=lambda name, **kw: created.update(kw) or PermissiveJob(
            kw.get("state") or {}),
    )
    eng._binding = lambda: {"network": "mainnet"}
    eng.request_bridge(target_micros=5_000_000, automatic=True)
    assert created["state"].get("rebalance") is True
    assert not created["state"].get("manual"), (
        "automatic bridges must NOT be stamped manual")
    assert "rebalance" in created["event_message"]

    created.clear()
    eng.request_bridge(target_micros=5_000_000)
    assert created["state"].get("manual") is True
    assert "manual" in created["event_message"]


def test_worker_passes_automatic_to_both_job_actuators():
    from types import SimpleNamespace

    w = _worker_with(PARAMS)
    eng = FakeEngine()
    w._maybe_rebalance(eng)                          # USDC excess -> bridge
    assert eng.calls == [("bridge", 50_000_000, True)]

    w2 = _worker_with(PARAMS)
    eng2 = FakeEngine()
    eng2._hot_cache["usdc_micros"] = 10_000_000
    eng2._resolve_cat_wallet_id = lambda: 7
    eng2._wallet = SimpleNamespace(
        get_wallet_balance=lambda wid: {"spendable_balance": 10 ** 9})
    w2._maybe_rebalance(eng2)
    # 90-USDC deficit grossed up by the 30 bps tip: ceil(90e6/0.9970)//1000.
    assert eng2.calls == [("unwrap", 90_270, eng2._hot_address, True)]


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
