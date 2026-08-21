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
    assert "_OUR_BOOK_WINDOW_H" in body


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
