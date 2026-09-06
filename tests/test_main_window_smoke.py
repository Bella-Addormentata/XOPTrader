"""Construct the main window.

No test did this before, so the entire suite passed green while the GUI could
not start at all: an inserted method landed between an existing
``@staticmethod`` and ``_create_page_widget``, stealing the decorator. Every
page then received ``self`` as its ``widget_class`` and construction raised
``TypeError: _create_page_widget() got multiple values for argument
'scrollable'``.

A smoke test is a blunt instrument, but it is the one that catches a whole
class of wiring and decorator damage that unit tests around the edges miss.
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


def test_the_main_window_constructs(app):
    from gui.widgets.main_window import MainWindow

    window = MainWindow()
    try:
        assert window._stacked.count() > 0, "no pages were built"
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_every_page_is_a_widget_not_a_bound_method_artifact(app):
    """The decorator bug produced pages built from the wrong argument.

    Asserting each page is a real QWidget catches that even if construction
    somehow survives.
    """
    from PySide6.QtWidgets import QWidget
    from gui.widgets.main_window import MainWindow

    window = MainWindow()
    try:
        for index in range(window._stacked.count()):
            page = window._stacked.widget(index)
            assert isinstance(page, QWidget), f"page {index} is {type(page)}"
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_the_page_builder_stays_a_staticmethod():
    """It takes no self, but is called as self._create_page_widget(...).

    Without the decorator Python binds self to widget_class and every page
    is built from the wrong argument.
    """
    from gui.widgets.main_window import MainWindow

    assert isinstance(
        MainWindow.__dict__["_create_page_widget"], staticmethod
    ), "_create_page_widget lost its @staticmethod"


def _health_payload(**health) -> dict:
    base = {
        "block_height": 9_184_000.0,
        "node_connected": 1.0,
        "node_synced": 1.0,
        "node_syncing": 0.0,
        "wallet_connected": 0.0,
        "wallet_synced": 0.0,
        "wallet_syncing": 0.0,
    }
    base.update(health)
    return {
        "health": base,
        "metrics_connected": True,
        "pnl": {},
        "offers": {},
        "risk": {},
        "market_data": {},
        "wallet_balances": {},
    }


def test_a_bridge_update_survives_a_zero_wallet_connected_gauge(app):
    """[S33 2026-09-05] The wallet fallback read a local bound 60 lines
    LOWER, so ``bool(wallet_balances)`` raised UnboundLocalError whenever the
    gauge was 0.0 or absent -- i.e. in the disconnected state the fallback
    exists to cover.  The slot has no try/except, so the exception escaped
    into Qt and every refresh after it (status bar, block label, dashboard,
    wallet and order-book panels) was skipped, freezing the window.

    The tooltip assertion is what makes this non-vacuous: it is written well
    past the crash site, so a swallowed exception or an early return keeps it
    stale and the test still fails.
    """
    from gui.widgets.main_window import MainWindow

    window = MainWindow()
    try:
        try:
            window._on_bridge_data(_health_payload(wallet_connected=0.0))
        except UnboundLocalError as exc:  # pragma: no cover - the defect
            pytest.fail(f"_on_bridge_data raised {exc!r}")

        tooltip = window._block_label.toolTip()
        assert "Wallet: Disconnected" in tooltip, (
            f"the status refresh past the wallet block did not run: {tooltip!r}"
        )
        assert "9,184,000" in tooltip
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


# ---------------------------------------------------------------------------
# Legacy-engine compatibility for the wallet dot.
#
# [S33 2026-09-05] This used to be a retained-balance fallback in the
# indicator itself, and it masked an explicit disconnect: WalletService's
# cache is merge-only and never cleared on a failed fetch, so
# `bool(wallet_balances)` is permanently true after the first success and
# overrode the authoritative wallet_connected = 0 that EngineBridge writes.
# The compat now lives a layer down, in MetricsService.get_health(), where it
# can distinguish "the engine never published the gauge" (fall back) from
# "the engine published 0" (report 0) -- a distinction the indicator, which
# only ever sees the resulting float, cannot make.
# ---------------------------------------------------------------------------

# An engine predating the wallet_connected gauge: note the absence.
_LEGACY_SCRAPE = """\
# HELP xop_node Node and wallet connectivity and sync status
# TYPE xop_node gauge
xop_node{metric="block_height"} 9184000
xop_node{metric="synced"} 1
xop_node{metric="wallet_synced"} 1
"""

# The same engine, plus an explicit "the wallet is unreachable" reading.
_DISCONNECTED_SCRAPE = _LEGACY_SCRAPE + 'xop_node{metric="wallet_connected"} 0\n'


def _health_from_scrape(sample_text: str) -> dict:
    """Run a real Prometheus body through the real MetricsService getter."""
    from gui.services.metrics_service import (
        MetricsService,
        _parse_prometheus_text,
    )

    service = MetricsService()
    try:
        service._latest = _parse_prometheus_text(sample_text)
        return service.get_health()
    finally:
        service.stop()
        service.deleteLater()


def test_an_engine_predating_the_wallet_connected_gauge_still_reads_connected(app):
    """Legacy compat, pinned end to end: absent gauge + a synced wallet."""
    from gui.widgets.main_window import MainWindow

    assert "wallet_connected" not in _LEGACY_SCRAPE, "premise: the gauge is absent"
    health = _health_from_scrape(_LEGACY_SCRAPE)
    assert health["wallet_connected"] == 1.0, (
        "get_health() dropped the legacy wallet_synced fallback"
    )

    window = MainWindow()
    try:
        payload = _health_payload()
        payload["health"] = health

        window._on_bridge_data(payload)

        assert "Wallet: Synced" in window._block_label.toolTip(), (
            "a legacy engine's synced wallet no longer reads as connected"
        )
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_an_explicitly_zero_wallet_gauge_is_not_rescued_by_that_fallback(app):
    """The other half: the fallback must not re-admit the masked disconnect.

    ``wallet_synced`` is a stale engine gauge (written only inside
    step_manage_offers), so it holds its last value through an outage; the
    default must apply to a MISSING sample only, never override a published
    zero.
    """
    from gui.widgets.main_window import MainWindow

    health = _health_from_scrape(_DISCONNECTED_SCRAPE)
    assert health["wallet_synced"] == 1.0, "premise: the stale synced gauge is 1"
    assert health["wallet_connected"] == 0.0, (
        "the legacy default overrode an explicit wallet_connected = 0"
    )

    window = MainWindow()
    try:
        payload = _health_payload()
        payload["health"] = health

        window._on_bridge_data(payload)

        assert "Wallet: Disconnected" in window._block_label.toolTip()
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()
