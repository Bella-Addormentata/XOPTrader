"""has_data() must answer "can I trust the gauges NOW", not "ever".

The venue switch asks it exactly one question: is a zero in the offer
counters a real zero, or an artefact of not being able to see? Getting that
backwards renders DEXIE OFF over a book that may still be takeable, which is
the failure the off-means-flat contract exists to prevent.

This is a REGRESSION test in the strict sense. The behaviour was claimed
fixed in an earlier review round and only the docstring changed; the
predicate still returned `bool(self._latest)`, which `_on_failure()`
deliberately does not clear.
"""

from __future__ import annotations

import pytest

pytest.importorskip("PySide6")

from gui.services.metrics_service import MetricsService  # noqa: E402


@pytest.fixture
def svc():
    s = MetricsService.__new__(MetricsService)
    from PySide6.QtCore import QMutex
    s._mutex = QMutex()
    s._latest = {}
    s._connected = False
    return s


def test_no_scrape_yet_is_not_data(svc):
    assert not svc.has_data()


def test_a_landed_scrape_while_connected_is_data(svc):
    svc._latest = {"xop_offers_pending": 0.0}
    svc._connected = True
    assert svc.has_data()


def test_a_disconnect_stops_the_cached_gauges_being_authoritative(svc):
    """The whole point. `_latest` is RETAINED on failure by design -- the
    dashboard should keep showing the last known figures rather than blank --
    so the connection state is what has to be consulted here."""
    svc._latest = {"xop_offers_pending": 0.0}
    svc._connected = True
    assert svc.has_data()

    svc._connected = False          # what _on_failure() does
    assert svc._latest, "the fixture must keep _latest, as _on_failure does"
    assert not svc.has_data(), (
        "a stale pending==0 would render DEXIE OFF over a live book")


def test_reconnecting_makes_them_authoritative_again(svc):
    svc._latest = {"xop_offers_pending": 3.0}
    svc._connected = False
    assert not svc.has_data()
    svc._connected = True
    assert svc.has_data()
