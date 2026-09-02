from scripts.permuto_field_monitor import DEFAULT_RING_PCT, active_ring_pct


def test_field_monitor_reads_the_active_venue_ring():
    assert active_ring_pct({"vol_aggressive_ring_pct": "1.5"}) == (
        1.5, "venue")


def test_field_monitor_labels_its_default_ring_fallback():
    assert active_ring_pct({}) == (DEFAULT_RING_PCT, "default")
    assert active_ring_pct({"vol_aggressive_ring_pct": float("nan")}) == (
        DEFAULT_RING_PCT, "default")