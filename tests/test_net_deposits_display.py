"""[WARP-MILLIETH 2026-10-24] The dashboard's Net Deposits figure.

The card summed bridge rows' mojos and divided by 1,000, which is the dollar
figure only while every flow is wUSDC.b at $1.00.  The engine now also books
wmilliETH.b warp flows, at its own USD mark, with the signed USD in each
row's note; the card sums that field, exactly as the engine rebuilds Net
Deposits on restart.  Runs the real query against a temporary SQLite file.
"""

from __future__ import annotations

import os
import sqlite3
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402

from gui.services.database_service import _parse_bridge_flow_usd  # noqa: E402

WUSDC = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"
WMILLIETH = "f322a205c034fe28681829fa5a2e483ac421f0952eb1292945c8db06e0a471a6"


@pytest.fixture(scope="module")
def app():
    yield QApplication.instance() or QApplication(sys.argv)


def _note(flow_usd: float, px: float, job: int) -> str:
    # accounting::bridge_note's format.
    kind = "deposit" if flow_usd >= 0 else "withdrawal"
    return (f"warp bridge {kind}; flow_usd={flow_usd:.10f}; "
            f"px_usd_per_unit={px:.10f}; job={job}")


def _seed(path: str, rows) -> None:
    con = sqlite3.connect(path)
    con.execute("""CREATE TABLE trade_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp TEXT, trade_id TEXT,
        pair_name TEXT, side TEXT, price_mojos INTEGER, size_mojos INTEGER,
        fee_mojos INTEGER DEFAULT 0, cost_basis_mojos INTEGER,
        realized_pnl_mojos INTEGER, block_height INTEGER, offer_hash TEXT,
        acquisition_ts TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP)""")
    con.execute("""CREATE TABLE ledger_entries (
        event_type TEXT, event_id TEXT, leg TEXT, asset_id TEXT,
        delta_mojos INTEGER, note TEXT)""")
    con.executemany(
        "INSERT INTO ledger_entries VALUES (?,?,?,?,?,?)", rows)
    con.commit()
    con.close()


def _display(tmp_path, rows) -> dict:
    from gui.services.database_service import _DatabaseWorker

    db = tmp_path / "pnl.db"
    _seed(str(db), rows)
    worker = _DatabaseWorker()
    worker.open(str(db))
    captured: dict = {}
    worker.pnl_display_ready.connect(lambda d: captured.update(d))
    worker.fetch_pnl_display("")
    return captured


def test_a_wmillieth_unwrap_counts_at_its_usd_value(app, tmp_path):
    out = _display(tmp_path, [
        ("bridge_deposit", "bridge:job:2:a", "bridge", WUSDC, 4_985,
         _note(4.985, 1.0, 2)),
        # 0.547 wmilliETH.b unwrapped at $3.95: -$2.16, not -$0.547.
        ("bridge_withdrawal", "bridge:job:9:b", "bridge", WMILLIETH, -547,
         _note(-2.16065, 3.95, 9)),
        ("fill", "trade:1", "base", WUSDC, 100, ""),     # not a bridge row
    ])
    assert out["bridge_flows"] == 2
    assert out["net_deposits_usd"] == pytest.approx(4.985 - 2.16065)


def test_no_ledger_table_is_no_flows(app, tmp_path):
    from gui.services.database_service import _DatabaseWorker

    db = tmp_path / "fresh.db"
    con = sqlite3.connect(str(db))
    con.execute("CREATE TABLE trade_log (realized_pnl_mojos INTEGER, "
                "created_at TEXT, pair_name TEXT, side TEXT, price_mojos "
                "INTEGER, size_mojos INTEGER)")
    con.commit()
    con.close()
    worker = _DatabaseWorker()
    worker.open(str(db))
    captured: dict = {}
    worker.pnl_display_ready.connect(lambda d: captured.update(d))
    worker.fetch_pnl_display("")
    assert captured["bridge_flows"] == 0
    assert captured["net_deposits_usd"] == 0.0


@pytest.mark.parametrize("note,expected", [
    (_note(-2.16065, 3.95, 9), -2.16065),
    (_note(4.985, 1.0, 2), 4.985),
    ("", 0.0),
    ("hand edit", 0.0),
    ("flow_usd=", 0.0),
    ("flow_usd=nan; x", 0.0),
    ("flow_usd=1e13", 0.0),          # the engine's magnitude bound
    (None, 0.0),
])
def test_the_note_parser_matches_the_engines(note, expected):
    assert _parse_bridge_flow_usd(note) == pytest.approx(expected)
