"""Regression tests for SQL-NULL handling in the GUI.

``dict.get(key, default)`` returns ``None`` — not *default* — when the key
is PRESENT with the value ``None``.  Widgets read rows produced by
``SELECT *``, so every nullable column arrives exactly that way.  509 of
the 1,486 ``trade_log`` rows have NULL ``cost_basis_mojos`` /
``realized_pnl_mojos`` (fills backfilled from chain history), 315 of them
inside the newest 1,000 the GUI loads — which made
``TradeLogWidget._populate_table`` raise ``TypeError`` in production.

Runs headless (``QT_QPA_PLATFORM=offscreen``) under pytest or directly:

    .venv/Scripts/python.exe tests/test_gui_null_safety.py
"""

from __future__ import annotations

import csv
import io
import os
import sys
from pathlib import Path

# Must be set before PySide6 imports a platform plugin.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402

from gui.utils import NULL_DISPLAY, num, opt_num, text  # noqa: E402
from gui.widgets.order_panel import OrderPanel  # noqa: E402
from gui.widgets.reports import ReportsWidget  # noqa: E402
from gui.widgets.trade_log import TradeLogWidget  # noqa: E402


@pytest.fixture(scope="session")
def qapp():
    """A single offscreen QApplication for the whole session."""
    app = QApplication.instance() or QApplication([])
    yield app


# ---------------------------------------------------------------------------
# Row fixtures — shaped exactly like `SELECT * FROM trade_log`
# ---------------------------------------------------------------------------

def _trade(**over):
    """A complete trade_log row; override any column via keyword."""
    row = {
        "id": 1,
        "timestamp": "2026-08-04 12:00:00",
        "trade_id": "a" * 40,
        "pair_name": "XCH/wUSDC.b",
        "side": "bid",
        "price_mojos": 12_500_000_000_000,
        "size_mojos": 1_000_000_000_000,
        "fee_mojos": 5_000,
        "cost_basis_mojos": 11_000_000_000_000,
        "realized_pnl_mojos": 1_500_000_000_000,
        "block_height": 7_100_000,
        "offer_hash": None,
        "acquisition_ts": None,
        "created_at": "2026-08-04 12:00:01",
    }
    row.update(over)
    return row


# The 2026-08-01/08-02 backfilled shape: key PRESENT, value NULL.
BACKFILLED = _trade(
    trade_id="b" * 40,
    cost_basis_mojos=None,
    realized_pnl_mojos=None,
)


# ---------------------------------------------------------------------------
# gui.utils helpers
# ---------------------------------------------------------------------------

def test_num_substitutes_default_for_present_none():
    """The exact defect: a present-but-NULL key must yield the default."""
    row = {"cost_basis_mojos": None}
    assert row.get("cost_basis_mojos", 0) is None, "premise of the bug"
    assert num(row, "cost_basis_mojos", 0) == 0
    assert num(row, "absent_key", 7) == 7
    assert num(row, "cost_basis_mojos", 0) + 1 == 1  # arithmetic is safe


def test_text_always_returns_str():
    assert text({"side": None}, "side") == ""
    assert text({"side": None}, "side").lower() == ""
    assert text({"side": "bid"}, "side") == "bid"
    assert text({"block_height": 12}, "block_height") == "12"


def test_opt_num_keeps_null_visible():
    assert opt_num({"realized_pnl_mojos": None}, "realized_pnl_mojos") is None
    assert opt_num({}, "realized_pnl_mojos") is None
    assert opt_num({"realized_pnl_mojos": 0}, "realized_pnl_mojos") == 0


# ---------------------------------------------------------------------------
# TradeLogWidget
# ---------------------------------------------------------------------------

def test_populate_table_survives_null_row(qapp):
    """The live crash: NULL cost basis / realized P&L must not raise."""
    w = TradeLogWidget()
    w._populate_table([_trade(), BACKFILLED])  # must not raise
    assert w._table.rowCount() == 2


def test_null_columns_render_as_em_dash_not_zero(qapp):
    """A backfilled fill must not read as a zero-profit trade."""
    w = TradeLogWidget()
    w._populate_table([BACKFILLED])
    cost_basis = w._table.item(0, 7)
    realized = w._table.item(0, 8)
    assert cost_basis.text() == NULL_DISPLAY
    assert realized.text() == NULL_DISPLAY
    # Visually distinct and explained.
    assert "0.0000" not in cost_basis.text()
    assert cost_basis.toolTip()
    assert realized.toolTip()


def test_non_null_row_still_formats_numerically(qapp):
    w = TradeLogWidget()
    w._populate_table([_trade()])
    assert w._table.item(0, 7).text() == "11.0000"
    assert w._table.item(0, 8).text() == "+1.5000"


def test_summary_excludes_null_pnl(qapp):
    """NULL P&L is unknown, not a loss: it must not dilute the win rate."""
    trades = [
        _trade(realized_pnl_mojos=2_000_000_000_000),   # win
        _trade(realized_pnl_mojos=-1_000_000_000_000),  # loss
        BACKFILLED,                                     # unknown
    ]
    w = TradeLogWidget()
    w._update_summary(trades)  # must not raise
    assert "Total trades: 3" in w._lbl_total_trades.text()
    assert "1 without P&L" in w._lbl_total_trades.text()
    # 1 win out of the 2 PRICED trades, not out of 3.
    assert "50.0%" in w._lbl_win_rate.text()
    assert "of 2" in w._lbl_win_rate.text()
    # Total is 2.0 - 1.0 XCH; the NULL contributes nothing.
    assert "1.0000" in w._lbl_total_pnl.text()


def test_summary_all_null_pnl(qapp):
    w = TradeLogWidget()
    w._update_summary([BACKFILLED, BACKFILLED])
    assert w._lbl_win_rate.text().endswith(NULL_DISPLAY)


def test_export_csv_writes_empty_cell_for_null(qapp, tmp_path):
    """A 0 in the CSV would silently understate profit in a spreadsheet."""
    w = TradeLogWidget()
    w._all_trades = [BACKFILLED]
    # Widen the date filter so the row survives _filtered_trades().
    from PySide6.QtCore import QDate

    w._date_from.setDate(QDate(2000, 1, 1))
    w._date_to.setDate(QDate(2100, 1, 1))

    out = tmp_path / "trades.csv"
    w.export_csv(str(out))
    rows = list(csv.reader(io.StringIO(out.read_text(encoding="utf-8"))))
    assert len(rows) == 2, rows
    header, data = rows
    assert data[header.index("cost_basis_xch")] == ""
    assert data[header.index("realized_pnl_xch")] == ""


def test_filters_survive_null_strings(qapp):
    w = TradeLogWidget()
    w._all_trades = [_trade(side=None, pair_name=None, timestamp=None)]
    w._rebuild_pair_combo()      # must not raise
    w._filtered_trades()          # must not raise


def test_full_load_path_with_nulls(qapp):
    """load_trades -> render -> summary, end to end, on a NULL-bearing set."""
    from PySide6.QtCore import QDate

    w = TradeLogWidget()
    w.show()  # load_trades defers the render while hidden
    w._date_from.setDate(QDate(2000, 1, 1))
    w._date_to.setDate(QDate(2100, 1, 1))
    w.load_trades([_trade(), BACKFILLED])
    assert w._table.rowCount() == 2


# ---------------------------------------------------------------------------
# ReportsWidget / OrderPanel
# ---------------------------------------------------------------------------

def test_reports_top_trades_null_cost_basis(qapp):
    """top_trades/worst_trades are the only un-COALESCE'd report rows."""
    w = ReportsWidget()
    w._fill_trades_table(w._best_table, [
        {
            "timestamp": "2026-08-04 12:00:00",
            "pair_name": "XCH/wUSDC.b",
            "side": "bid",
            "price_mojos": 12_500_000_000_000,
            "size_mojos": 1_000_000_000_000,
            "realized_pnl_mojos": 1_000_000_000,
            "cost_basis_mojos": None,
        },
    ])  # must not raise
    assert w._best_table.item(0, 6).text() == NULL_DISPLAY


def test_order_panel_null_offer_columns(qapp):
    """offer_log has nullable tier / resolved_at / fee columns."""
    w = OrderPanel()
    offers = [{
        "id": 1,
        "offer_id": "c" * 40,
        "pair_name": "XCH/wUSDC.b",
        "side": "bid",
        "price_mojos": 12_500_000_000_000,
        "size_mojos": 1_000_000_000_000,
        "tier": None,
        "status": "pending",
        "created_block": 7_100_000,
        "resolved_block": None,
        "resolved_at": None,
        "fee_mojos": None,
        "cancel_reason": None,
    }]
    w._populate_table(offers)   # must not raise
    w._all_offers = offers
    w._update_summary()          # must not raise
    assert w._table.rowCount() == 1


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
