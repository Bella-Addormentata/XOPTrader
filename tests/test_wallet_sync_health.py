"""The GUI's own wallet-RPC reading must be authoritative in BOTH directions.

EngineBridge.get_all_data() merges WalletService.get_sync_status() over the
engine's Prometheus health gauges.  Those gauges are weak: xop_node
wallet_connected is only ChiaRPCBase::is_open() (the client object was opened
and never closed), and wallet_synced_ is written solely inside
Engine::step_manage_offers, so it holds its last value indefinitely whenever
an earlier gate short-circuits Step 8.  Merging only the OPTIMISTIC branches
therefore left a desynced or unreachable wallet still painted
"Wallet: Synced" green off a stale gauge -- the fail-open shape.
"""

from __future__ import annotations

import pytest


class _StubMetrics:
    """Engine gauges frozen in the STALE-OPTIMISTIC state."""

    is_connected = True

    def __init__(self, health: dict[str, float]) -> None:
        self._health = health

    def get_health(self) -> dict[str, float]:
        return dict(self._health)

    def get_pnl(self) -> dict:
        return {}

    def get_offers_summary(self) -> dict:
        return {}

    def get_risk(self) -> dict:
        return {}

    def get_market_data(self, pair: str) -> dict:
        return {}

    def get_analysis(self, pairs) -> dict:
        return {}

    def get_spendable_reserve(self) -> float:
        return 0.0

    def get_stuck_offers(self) -> list:
        return []

    def get_fees_paid_24h(self) -> float:
        return 0.0


class _StubWallet:
    def __init__(self, sync_status: dict) -> None:
        self._sync_status = sync_status

    def get_sync_status(self) -> dict:
        return dict(self._sync_status)

    def get_balances(self) -> dict:
        return {}


class _StubConfig:
    def get_pairs(self) -> list:
        return []

    def get_full_config(self) -> dict:
        return {}


class _StubDatabase:
    def query_last_trade_prices(self) -> None:
        return None


class _StubWarp:
    def get_snapshot(self) -> dict:
        return {}


# The engine reading that must NOT survive a first-hand "no" from the GUI's
# own wallet RPC.
_STALE_OPTIMISTIC = {
    "block_height": 9_184_000.0,
    "node_connected": 1.0,
    "node_synced": 1.0,
    "node_syncing": 0.0,
    "wallet_connected": 1.0,
    "wallet_synced": 1.0,
    "wallet_syncing": 0.0,
}


def _bridge(sync_status: dict):
    from gui.services.engine_bridge import EngineBridge

    bridge = EngineBridge.__new__(EngineBridge)
    bridge._metrics_svc = _StubMetrics(_STALE_OPTIMISTIC)
    bridge._wallet_svc = _StubWallet(sync_status)
    bridge._config_svc = _StubConfig()
    bridge._database_svc = _StubDatabase()
    bridge._warp_svc = _StubWarp()
    bridge._last_trade_prices = {}
    bridge._last_trade_cache_ts = 0.0
    bridge._last_trade_request_ts = 0.0
    bridge._last_data = {}
    bridge._bot_status = "Running"
    return bridge


@pytest.fixture
def health_of():
    def _health_of(sync_status: dict) -> dict[str, float]:
        return _bridge(sync_status).get_all_data()["health"]

    return _health_of


def test_unsynced_direct_status_overrides_stale_engine_gauges(health_of):
    """connected=True / synced=False / syncing=False is the case the old
    asymmetric merge dropped entirely: neither branch fired, so the stale
    engine gauge kept the badge green."""
    health = health_of({"connected": True, "synced": False, "syncing": False})
    assert health["wallet_synced"] == 0.0, (
        "a first-hand 'not synced' RPC reading must clear the engine's "
        "stale wallet_synced gauge -- otherwise the GUI keeps painting "
        "'Wallet: Synced' green over a desynced wallet"
    )
    assert health["wallet_syncing"] == 0.0
    assert health["wallet_connected"] == 1.0


def test_disconnected_direct_status_clears_connected_gauge(health_of):
    """A hard RPC failure must not read as connected just because the
    engine's ChiaRPC client object is still nominally open."""
    health = health_of({"connected": False, "synced": False, "syncing": False})
    assert health["wallet_connected"] == 0.0
    assert health["wallet_synced"] == 0.0
    assert health["wallet_syncing"] == 0.0


def test_syncing_direct_status_is_reported_as_syncing(health_of):
    health = health_of({"connected": True, "synced": False, "syncing": True})
    assert health["wallet_connected"] == 1.0
    assert health["wallet_synced"] == 0.0
    assert health["wallet_syncing"] == 1.0


def test_synced_direct_status_wins_over_a_simultaneous_syncing_flag(health_of):
    """Precedence is unchanged from before the fix: synced beats syncing."""
    health = health_of({"connected": True, "synced": True, "syncing": True})
    assert health["wallet_synced"] == 1.0
    assert health["wallet_syncing"] == 0.0


def test_absent_direct_status_keeps_engine_gauges(health_of):
    """Before the first wallet fetch completes the cached status is {}, and
    the engine gauges must still supply the display."""
    health = health_of({})
    assert health["wallet_connected"] == 1.0
    assert health["wallet_synced"] == 1.0
    assert health["wallet_syncing"] == 0.0
