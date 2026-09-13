"""The status-bar / dashboard connectivity indicators.

[S33 2026-09-05] These dots exist to report LIVENESS. Two of them were
derived from remembered state instead, so a real outage rendered as a
healthy-ish UI:

* the node dot ORed in ``block_height > 0 and metrics_connected``, and the
  engine keeps publishing a (wallet-sourced) height and stays scrapeable
  right through a full-node outage -- so an explicit ``node_connected == 0``
  was overridden and the outage showed as yellow "Not Synced" forever;
* the Dexie dot ORed in ``bool(market_data)``, which MetricsService retains
  by design after a failed scrape and which carries an entry per configured
  pair even before the first scrape -- so the dot was pinned green;
* the wallet dot ORed in ``bool(wallet_balances)``, which comes from
  WalletService's merge-only ``_cached`` (deliberately never cleared on a
  failed fetch, so a single timed-out RPC cannot erase known wallets) -- so
  after the first successful fetch it was permanently true and overrode the
  authoritative ``wallet_connected = 0`` EngineBridge writes from the direct
  wallet RPC, rendering an unreachable wallet yellow "Not Synced".

Both are asserted here through the real widgets rather than the source text,
so the tests fail if the masking returns in any form.
"""

from __future__ import annotations

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402


@pytest.fixture(scope="module")
def app():
    instance = QApplication.instance() or QApplication(sys.argv)
    instance.setStyle("Fusion")
    yield instance


@pytest.fixture(scope="module")
def window(app):
    """One window for the module: construction is heavy, and every call to
    ``_on_bridge_data`` recomputes the labels from scratch."""
    from gui.widgets.main_window import MainWindow

    win = MainWindow()
    try:
        yield win
    finally:
        win.close()
        win.deleteLater()
        app.processEvents()


def _payload(**health_overrides) -> dict:
    """The snapshot EngineBridge emits, with a live-looking baseline.

    ``block_height`` is non-zero and ``market_data`` non-empty on purpose:
    those are precisely the retained values that used to mask an outage.
    """
    health = {
        "block_height": 9_184_000.0,
        "node_connected": 1.0,
        "node_synced": 1.0,
        "node_syncing": 0.0,
        "wallet_connected": 1.0,
        "wallet_synced": 1.0,
        "wallet_syncing": 0.0,
        "dexie_connected": 1.0,
    }
    health.update(health_overrides)
    return {
        "health": health,
        "metrics_connected": True,
        "pnl": {},
        "offers": {},
        "risk": {},
        "wallet_balances": {},
        "market_data": {
            "XCH/wUSDC.b": {
                "mid_price": 1_486_000_000_000.0,
                "spread_bps": 42.0,
                "volume_24h": 1.0,
            }
        },
    }


def _dot_label(window, service: str) -> str:
    dashboard = window._unwrap(window._dashboard)
    _dot, label = dashboard._conn_dots[service]
    return label.text()


# -- Node indicator (c18) ------------------------------------------------


def test_an_explicit_node_disconnect_is_not_masked_by_a_retained_height(window):
    """The outage state: node gauges 0, engine still scrapeable, height kept.

    That is the only state in which the removed disjunct was load-bearing,
    so it is the only state that can prove it is gone.
    """
    payload = _payload(node_connected=0.0, node_synced=0.0, node_syncing=0.0)
    assert payload["health"]["block_height"] > 0, "premise: the height is retained"
    assert payload["metrics_connected"] is True, "premise: the engine is scrapeable"

    window._on_bridge_data(payload)

    assert "Full Node: Disconnected" in window._block_label.toolTip()
    assert _dot_label(window, "Full Node") == "Full Node: Disconnected"


def test_a_live_node_still_reads_synced(window):
    """Guards against 'fixing' the mask by hard-coding disconnected."""
    window._on_bridge_data(_payload())

    assert "Full Node: Synced" in window._block_label.toolTip()
    assert _dot_label(window, "Full Node") == "Full Node: Synced"


def test_a_syncing_node_reads_syncing_not_not_synced(window):
    """The state the node-sync work exists to make reachable.

    The engine now separates node_synced from node_syncing; before that, a
    node catching up on the chain and a node stalled at an old peak both
    rendered the same yellow "Not Synced", so the operator could not tell a
    self-healing state from one needing intervention.
    """
    window._on_bridge_data(
        _payload(node_connected=1.0, node_synced=0.0, node_syncing=1.0)
    )

    assert "Full Node: Syncing..." in window._block_label.toolTip()
    assert _dot_label(window, "Full Node") == "Full Node: Syncing..."


def test_an_engine_predating_the_node_connected_gauge_still_reads_connected(window):
    """Legacy compat: MetricsService defaults node_connected from the legacy
    ``synced`` gauge, so node_synced alone must still mean connected."""
    payload = _payload(node_synced=1.0)
    payload["health"].pop("node_connected")

    window._on_bridge_data(payload)

    assert "Full Node: Synced" in window._block_label.toolTip()


def test_a_dead_scrape_is_not_masked_by_retained_node_health(window):
    """[review round 8] The same masking as c18/c19, one layer up.

    MetricsService._on_failure() clears _connected but deliberately RETAINS
    _latest, so ``health`` keeps reporting the last good scrape for the whole
    outage.  A node that was synced when the endpoint died must not stay
    green through it.
    """
    payload = _payload()
    payload["metrics_connected"] = False
    assert payload["health"]["node_synced"] == 1.0, (
        "premise: the gauges are retained and still look healthy"
    )

    window._on_bridge_data(payload)

    assert _dot_label(window, "Full Node") == "Full Node: Disconnected"


def test_an_absent_metrics_liveness_key_fails_closed(window):
    """The DEFAULT is load-bearing, so it gets its own case.

    Every other payload in this file sets ``metrics_connected`` explicitly,
    so flipping the default would otherwise redden nothing.  A health
    indicator must fail closed -- deliberately unlike ``_metrics_live``,
    whose True default serves a P&L rate rather than an alarm.
    """
    payload = _payload()
    payload.pop("metrics_connected")

    window._on_bridge_data(payload)

    assert _dot_label(window, "Full Node") == "Full Node: Disconnected"


# -- Wallet indicator (c13) ----------------------------------------------


def _with_retained_balances(payload: dict) -> dict:
    """The state WalletService leaves behind after a failed fetch: the last
    known wallets are still in ``_cached``, and so still in the payload."""
    payload["wallet_balances"] = {
        "Chia Wallet": {"wallet_type": 0.0, "asset_id": "", "confirmed": 16.5},
    }
    return payload


def test_an_explicit_wallet_disconnect_is_not_masked_by_retained_balances(window):
    """The outage state: EngineBridge asserts 0 from the direct wallet RPC
    while the merge-only balance cache still holds the last good snapshot.

    That is the only state in which the removed disjunct was load-bearing.
    """
    payload = _with_retained_balances(
        _payload(wallet_connected=0.0, wallet_synced=0.0, wallet_syncing=0.0)
    )
    assert payload["wallet_balances"], "premise: the balances are retained"

    window._on_bridge_data(payload)

    assert "Wallet: Disconnected" in window._block_label.toolTip()
    assert _dot_label(window, "Wallet") == "Wallet: Disconnected"


def test_a_stale_synced_gauge_does_not_survive_the_disconnect_either(window):
    """``wallet_synced`` is written only inside step_manage_offers, so it
    holds its last value while an earlier gate short-circuits Step 8.  A
    disconnected wallet must read red whatever that stale gauge says."""
    payload = _with_retained_balances(
        _payload(wallet_connected=0.0, wallet_synced=1.0, wallet_syncing=0.0)
    )

    window._on_bridge_data(payload)

    assert _dot_label(window, "Wallet") == "Wallet: Disconnected"


def test_a_live_wallet_still_reads_synced(window):
    """Non-vacuity: the dot must still be able to say Synced."""
    window._on_bridge_data(_with_retained_balances(_payload()))

    assert "Wallet: Synced" in window._block_label.toolTip()
    assert _dot_label(window, "Wallet") == "Wallet: Synced"


# -- Dexie indicator (c19) -----------------------------------------------


def test_the_dexie_dot_goes_red_when_the_scrape_dies_despite_retained_market_data(window):
    payload = _payload()
    payload["metrics_connected"] = False
    assert payload["market_data"], "premise: the last snapshot is retained"

    window._on_bridge_data(payload)

    assert _dot_label(window, "Dexie") == "Dexie: Disconnected"


def test_the_dexie_dot_is_green_while_the_scrape_is_live(window):
    """Non-vacuity: the dot must still be able to say Connected."""
    window._on_bridge_data(_payload())

    assert _dot_label(window, "Dexie") == "Dexie: Connected"


# -- Dexie indicator, second form (review 3997548811) --------------------
#
# c19 replaced `or bool(market_data)` with `metrics_connected`, which made
# the dot report whether the GUI could scrape the ENGINE.  That is engine
# health, not venue reachability: a healthy engine whose every Dexie request
# fails kept the dot green.  The engine now publishes the venue signal.


def test_an_unreachable_dexie_is_not_masked_by_a_healthy_engine(window):
    """THE finding: engine up, scrape live, Dexie down."""
    payload = _payload(dexie_connected=0.0)
    assert payload["metrics_connected"] is True, "premise: the engine is scrapeable"
    assert payload["market_data"], "premise: the last snapshot is retained"

    window._on_bridge_data(payload)

    assert _dot_label(window, "Dexie") == "Dexie: Disconnected", (
        "a green dot here is the fail-open status bug: the GUI could reach "
        "the engine, the engine could not reach Dexie"
    )


def test_the_scrape_gate_still_applies_to_the_dexie_dot(window):
    """The engine signal does not REPLACE the scrape gate, it joins it.

    A retained dexie_connected=1 from a dead scrape is remembered state, and
    every dot in this method reports asserted liveness, never memory.
    """
    payload = _payload(dexie_connected=1.0)
    payload["metrics_connected"] = False

    window._on_bridge_data(payload)

    assert _dot_label(window, "Dexie") == "Dexie: Disconnected"


def test_an_engine_predating_the_dexie_gauge_fails_closed(window):
    """No legacy gauge exists to fall back to, unlike node_connected.

    An engine that cannot be asked whether Dexie is up is not evidence that
    Dexie is up, so the absent key reads as not-reachable rather than green.
    """
    payload = _payload()
    del payload["health"]["dexie_connected"]

    window._on_bridge_data(payload)

    assert _dot_label(window, "Dexie") == "Dexie: Disconnected"


def test_an_explicit_node_disconnect_beats_a_stale_synced_gauge(window):
    """The second masking route, found by review after the first fix landed.

    The first cut of the height-fallback fix replaced one disjunct with
    another -- ``node_connected >= 1.0 or node_synced >= 1.0`` -- which
    re-opened the same hole from the other side: an engine explicitly
    reporting ``node_connected = 0`` while ``node_synced`` was still 1 (a
    stale reading, or the legacy gauge) was painted connected again.

    The legacy fallback is a fallback for an ABSENT key, not a second vote.
    When the gauge is present its value wins, including a false one, which
    is what ``MetricsService.get_health()`` already assumes one layer down.
    """
    payload = _payload(node_connected=0.0, node_synced=1.0, node_syncing=0.0)
    assert payload["health"]["node_connected"] == 0.0, "premise: gauge present and false"
    assert payload["health"]["node_synced"] == 1.0, "premise: stale synced still 1"

    window._on_bridge_data(payload)

    assert "Full Node: Disconnected" in window._block_label.toolTip()
    assert _dot_label(window, "Full Node") == "Full Node: Disconnected"
