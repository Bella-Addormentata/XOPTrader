from scripts.permuto_field_monitor import (
    DEFAULT_RING_PCT,
    active_ring_pct,
    fetch_field,
)


def test_field_monitor_reads_the_active_venue_ring():
    assert active_ring_pct({"vol_aggressive_ring_pct": "1.5"}) == (
        1.5, "venue")


def test_field_monitor_reads_nested_venue_ring():
    assert active_ring_pct({"config": {"flags": {"vol_aggressive_ring_pct": "1.75"}}}) == (
        1.75, "venue")


def test_field_monitor_labels_its_default_ring_fallback():
    assert active_ring_pct({}) == (DEFAULT_RING_PCT, "default")
    assert active_ring_pct({"vol_aggressive_ring_pct": float("nan")}) == (
        DEFAULT_RING_PCT, "default")


def test_fetch_field_pages_until_empty_page_when_total_invalid(monkeypatch):
    import scripts.permuto_field_monitor as mod

    pages = [
        {"market_makers": [{"user_id": "u%d" % i} for i in range(20)], "market_makers_total": None},
        {"market_makers": [{"user_id": "u%d" % (i + 20)} for i in range(5)], "market_makers_total": None},
        {"market_makers": [], "market_makers_total": None},
    ]

    def mock_get(path, timeout=25):
        offset = int(path.split("offset=")[1].split("&")[0])
        idx = offset // 20
        return pages[idx] if idx < len(pages) else {"market_makers": []}

    monkeypatch.setattr(mod, "get", mock_get)
    rows, _ = fetch_field()
    assert len(rows) == 25