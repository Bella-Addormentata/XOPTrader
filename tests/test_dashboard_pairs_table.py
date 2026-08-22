"""The dashboard's per-pair table.

Like the activity feed beside it, `update_pairs_table` was defined and called
from nowhere, so the panel never showed a row. It reports OUR OWN book -- the
best prices we are currently resting -- not the third-party market.
"""

from __future__ import annotations

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402


@pytest.fixture(scope="module")
def app():
    yield QApplication.instance() or QApplication(sys.argv)


def _dash():
    from gui.widgets.dashboard import DashboardWidget
    return DashboardWidget()


def test_the_table_populates(app):
    dash = _dash()
    dash.update_pairs_table([{
        "pair": "XCH/wUSDC.b", "mid_price": 1.486, "spread_bps": 42.0,
        "inventory": 12.5, "bid": 1.48, "ask": 1.49,
        "fills_24h": 32, "pnl": 0.0013, "quote_symbol": "wUSDC.b",
    }])
    assert dash._pairs_table.rowCount() == 1
    assert dash._pairs_table.item(0, 0).text() == "XCH/wUSDC.b"


def test_an_absent_quote_is_not_shown_as_a_price_of_zero(app):
    """wmilliETH.b is bid-only, so its ask side has no quote at all."""
    dash = _dash()
    dash.update_pairs_table([{
        "pair": "wmilliETH.b/XCH", "mid_price": 1.63, "spread_bps": 0.0,
        "inventory": 0.0, "bid": 1.6347, "ask": 0.0,
        "fills_24h": 0, "pnl": 0.0, "quote_symbol": "XCH",
    }])
    assert dash._pairs_table.item(0, 5).text() == "\u2014"
    assert "0.000000" not in dash._pairs_table.item(0, 5).text()


def test_pnl_is_labelled_in_the_pairs_own_quote_currency(app):
    """realized_pnl is quote mojos: XCH/BYC settles in BYC, not XCH."""
    dash = _dash()
    dash.update_pairs_table([{
        "pair": "XCH/BYC", "mid_price": 1.5, "spread_bps": 10.0,
        "inventory": 1.0, "bid": 1.48, "ask": 1.51,
        "fills_24h": 8, "pnl": 0.0005, "quote_symbol": "BYC",
    }], xch_usd_rate=14.86)
    text = dash._pairs_table.item(0, 7).text()
    assert "BYC" in text
    # The XCH/USD rate must NOT be applied to a BYC-denominated figure.
    assert "$" not in text


def test_an_xch_quoted_pair_still_gets_its_usd_equivalent(app):
    dash = _dash()
    dash.update_pairs_table([{
        "pair": "wmilliETH.b/XCH", "mid_price": 1.63, "spread_bps": 5.0,
        "inventory": 0.0, "bid": 1.63, "ask": 0.0,
        "fills_24h": 0, "pnl": 0.25, "quote_symbol": "XCH",
    }], xch_usd_rate=14.86)
    assert "$" in dash._pairs_table.item(0, 7).text()


def test_our_book_query_excludes_never_resolved_rows():
    """Stale 'pending' rows made the book look self-crossed.

    Measured 2026-08-21: an XCH/BYC ask from 2026-08-13 and a bid from
    2026-08-08 were still pending, producing a best bid ABOVE the best ask.
    """
    from pathlib import Path
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "services", "database_service.py").read_text(encoding="utf-8")
    body = source.split("def fetch_pair_summary")[1].split("def fetch_reports")[0]
    assert "created_at >= ?" in body, "our-book query has no recency window"
    assert "our_book_window_hours(" in body, "window is not TTL-derived"


def test_the_two_tables_use_their_own_timestamp_formats():
    """offer_log stores "YYYY-MM-DD HH:MM:SS"; trade_log stores ISO with T.

    A cutoff in the wrong format compares as text and silently widens the
    window -- SQLite's datetime('now','-1 day') over trade_log returned 49
    rows where 35 were in range.
    """
    from pathlib import Path
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "services", "database_service.py").read_text(encoding="utf-8")
    body = source.split("def fetch_pair_summary")[1].split("def fetch_reports")[0]
    assert '"%Y-%m-%d %H:%M:%S"' in body      # offer_log
    assert '"%Y-%m-%dT%H:%M:%S"' in body      # trade_log


# ---------------------------------------------------------------------------
# Execute the query for real.
#
# The first version of these tests only built widget rows and grepped source,
# so a NameError inside fetch_pair_summary -- raised before either query ran,
# leaving the table permanently empty -- passed every check.
# ---------------------------------------------------------------------------

def _seed_db(path):
    import sqlite3
    from datetime import datetime, timedelta, timezone
    now = datetime.now(timezone.utc)
    recent_offer = now.strftime("%Y-%m-%d %H:%M:%S")          # offer_log format
    stale_offer = (now - timedelta(days=8)).strftime("%Y-%m-%d %H:%M:%S")
    recent_fill = now.strftime("%Y-%m-%dT%H:%M:%S.000Z")      # trade_log format

    con = sqlite3.connect(path)
    con.execute("""CREATE TABLE offer_log (pair_name TEXT, side TEXT,
                   price_mojos INTEGER, status TEXT, created_at TEXT)""")
    con.execute("""CREATE TABLE trade_log (pair_name TEXT, timestamp TEXT,
                   realized_pnl_mojos INTEGER)""")
    con.executemany(
        "INSERT INTO offer_log VALUES (?,?,?,?,?)",
        [
            ("XCH/BYC", "bid", 1_485_666_367_202, "pending", recent_offer),
            ("XCH/BYC", "ask", 1_510_309_000_000, "pending", recent_offer),
            # The ghost: eight days old, still 'pending'.  Including it would
            # make the best ask 1.4534 and cross the book.
            ("XCH/BYC", "ask", 1_453_390_027_906, "pending", stale_offer),
        ],
    )
    con.executemany("INSERT INTO trade_log VALUES (?,?,?)",
                    [("XCH/BYC", recent_fill, 556)])
    con.commit()
    con.close()


def test_the_query_runs_and_returns_our_book(app, tmp_path):
    from gui.services.database_service import _DatabaseWorker

    db = tmp_path / "t.db"
    _seed_db(str(db))
    worker = _DatabaseWorker()
    worker.open(str(db))

    captured = {}
    worker.pair_summary_ready.connect(lambda d: captured.update(d))
    worker.fetch_pair_summary()          # must not raise

    assert "XCH/BYC" in captured, "the query produced nothing"
    ours = captured["XCH/BYC"]
    assert ours["bid_mojos"] == 1_485_666_367_202
    assert ours["fills_24h"] == 1
    assert ours["pnl_mojos"] == 556


def test_the_ghost_row_is_excluded_so_the_book_is_not_crossed(app, tmp_path):
    from gui.services.database_service import _DatabaseWorker

    db = tmp_path / "g.db"
    _seed_db(str(db))
    worker = _DatabaseWorker()
    worker.open(str(db))
    captured = {}
    worker.pair_summary_ready.connect(lambda d: captured.update(d))
    worker.fetch_pair_summary()

    ours = captured["XCH/BYC"]
    assert ours["ask_mojos"] == 1_510_309_000_000, "picked up the stale ask"
    assert ours["bid_mojos"] < ours["ask_mojos"], "our book is crossed"


def test_the_inventory_gauge_name_matches_the_engine():
    """The service looked up a name the engine never exported, so the
    Inventory column would have read zero for every pair."""
    from pathlib import Path
    root = Path(__file__).resolve().parent.parent
    exported = (root / "cpp" / "src" / "monitoring" / "metrics.cpp").read_text(
        encoding="utf-8")
    service = (root / "gui" / "services" / "metrics_service.py").read_text(
        encoding="utf-8")
    assert '.Name("xop_inventory_balance")' in exported
    assert '"xop_inventory_balance"' in service
    assert '"xop_inventory_balance_mojos"' not in service


def test_pnl_uses_the_quote_divisor_not_the_xch_one():
    """realized_pnl is quote mojos: a CAT quote divides by 1000, not 1e12."""
    from pathlib import Path
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "widgets", "main_window.py").read_text(encoding="utf-8")
    body = source.split("def _refresh_pairs_table")[1].split("def ")[0]
    assert 'mojos_per_unit_for_pair(pair_name, "quote")' in body


def test_the_asset_id_is_lowercased_to_match_the_published_label():
    """config.cpp lowercases ids (T3-29) before they become metric labels.

    ConfigService returns the YAML spelling and the Settings UI accepts
    uppercase, so an uppercase id would miss the case-sensitive lookup and
    render Inventory as 0 -- indistinguishable from a real zero balance.
    """
    from pathlib import Path
    root = Path(__file__).resolve().parent.parent
    cpp = (root / "cpp" / "src" / "config.cpp").read_text(encoding="utf-8")
    assert "to_lower(p.base_asset_id)" in cpp, "engine no longer normalises"
    bridge = (root / "gui" / "services" / "engine_bridge.py").read_text(
        encoding="utf-8")
    line = [l for l in bridge.split(chr(10)) if "base_id = str(" in l]
    assert line and ".lower()" in line[0], "asset id not normalised for lookup"


def test_a_last_trade_backfill_is_not_shown_as_the_mid_price(app):
    """The bridge backfills mid_price from the last fill and marks it.

    That fill has no recency bound -- XCH/wUSDC's most recent is from April --
    so displaying it would label a four-month-old trade as the current mid.
    """
    from pathlib import Path
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "widgets", "main_window.py").read_text(encoding="utf-8")
    body = source.split("def _refresh_pairs_table")[1].split("\n    def ")[0]
    assert 'mid_price_source' in body, "the source marker is ignored"

    # And the widget must render the resulting 0.0 as an em dash, never 0.000000.
    from gui.widgets.dashboard import DashboardWidget
    dash = DashboardWidget()
    dash.update_pairs_table([{
        "pair": "XCH/wUSDC", "mid_price": 0.0, "spread_bps": 0.0,
        "inventory": 0.0, "bid": 0.0, "ask": 0.0,
        "fills_24h": 0, "pnl": 0.0, "quote_symbol": "wUSDC",
    }])
    assert dash._pairs_table.item(0, 1).text() == "\u2014"


def test_the_pairs_table_is_actually_connected():
    """The original defect was an implemented method with no caller.

    Calling update_pairs_table directly cannot catch that, so assert the
    wiring itself -- mirroring the activity feed's connection test.
    """
    from pathlib import Path
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "widgets", "main_window.py").read_text(encoding="utf-8")
    assert "pair_summary_loaded.connect(self._on_pair_summary)" in source
    assert "db.query_pair_summary(" in source
    # And something must actually push rows into the widget.
    assert "dashboard.update_pairs_table(" in source


def test_the_book_window_follows_the_configured_ttl():
    """A fixed window has a false-negative mode.

    The engine permits an offer until a hard TTL of 2x the configured value,
    and Settings allows up to 1000 blocks (~10.4h), so a legitimately resting
    quote would age out of a fixed 6h window and its side would wrongly show
    as absent.
    """
    from gui.services.database_service import our_book_window_hours

    # Never narrower than the floor, whatever the config says.
    assert our_book_window_hours(0) >= 6.0
    assert our_book_window_hours(None) >= 6.0

    for ttl in (400, 1000):
        hard_ttl_hours = ttl * 2 * 18.75 / 3600.0
        assert our_book_window_hours(ttl) > hard_ttl_hours, (
            f"ttl={ttl}: window would hide an offer still inside its hard TTL"
        )
    # And it must actually widen, not sit at the floor for every setting.
    assert our_book_window_hours(1000) > our_book_window_hours(400)


def test_the_usd_rate_is_not_taken_from_a_stale_fallback():
    """The table refuses a last_trade mid; the P&L conversion must too.

    Otherwise a months-old fill reappears as a dollar figure, which is worse
    for looking authoritative.
    """
    from pathlib import Path
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "widgets", "main_window.py").read_text(encoding="utf-8")
    block = source.split("xch_usd = 0.0")[1].split("self._last_market_data")[0]
    assert 'mid_price_source' in block and "continue" in block, (
        "the XCH/USD rate still accepts a last_trade backfill"
    )
