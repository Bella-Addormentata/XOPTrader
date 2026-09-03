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
    assert active_ring_pct({"vol_aggressive_ring_pct": True}) == (
        DEFAULT_RING_PCT, "default")
    assert active_ring_pct({"vol_aggressive_ring_pct": False}) == (
        DEFAULT_RING_PCT, "default")
    assert active_ring_pct({"vol_aggressive_ring_pct": 0.0}) == (
        DEFAULT_RING_PCT, "default")
    assert active_ring_pct({"vol_aggressive_ring_pct": 10.0}) == (
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


def test_ring_state_computes_ask_ticks_on_empty_bid(monkeypatch):
    import scripts.permuto_field_monitor as mod

    def mock_get(path, timeout=25):
        if path == "/info/oracle":
            return {"prices": {"NVDA-VOL": 0.20, "QQQ-VOL": 0.10, "TSLA-VOL": 0.28}}
        if path == "/info/meta":
            return {"vol_aggressive_ring_pct": 2.0, "markets": [{"symbol": "QQQ-VOL-PERP", "tick_size": 0.0001}]}
        if "/info/l2/" in path:
            return {"bids": [], "asks": []}
        return {}

    monkeypatch.setattr(mod, "get", mock_get)
    st = mod.ring_state()
    assert st["QQQ-VOL-PERP"]["ask_ticks"] > 0
    assert st["QQQ-VOL-PERP"]["best_bid"] is None


def test_ring_state_falls_back_on_malformed_or_nonpositive_tick_size(monkeypatch):
    import scripts.permuto_field_monitor as mod

    def mock_get(path, timeout=25):
        if path == "/info/oracle":
            return {"prices": {"NVDA-VOL": 0.20, "QQQ-VOL": 0.10, "TSLA-VOL": 0.28}}
        if path == "/info/meta":
            return {
                "vol_aggressive_ring_pct": 2.0,
                "markets": [
                    {"symbol": "QQQ-VOL-PERP", "tick_size": float("nan")},
                    {"symbol": "NVDA-VOL-PERP", "tick_size": -0.01},
                    {"symbol": "TSLA-VOL-PERP", "tick_size": 0.0},
                ],
            }
        if "/info/l2/" in path:
            return {"bids": [], "asks": []}
        return {}

    monkeypatch.setattr(mod, "get", mock_get)
    st = mod.ring_state()
    for m in ("QQQ-VOL-PERP", "NVDA-VOL-PERP", "TSLA-VOL-PERP"):
        assert st[m]["tick_size"] == 0.0001
        assert st[m]["ask_ticks"] > 0