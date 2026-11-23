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
    assert out["options_flows"] == 0
    assert out["net_deposits_usd"] == pytest.approx(4.985 - 2.16065)


def _options_note(flow_usd: float, px: float, ref: str) -> str:
    # accounting::options_note's format.
    kind = "deposit" if flow_usd >= 0 else "withdrawal"
    return (f"options sub-account {kind}; flow_usd={flow_usd:.10f}; "
            f"px_usd_per_unit={px:.10f}; ref={ref}")


def test_options_sub_account_flows_are_external_capital_too(app, tmp_path):
    """[OPTIONS-CAPITAL 2026-11-23] A top-up of the options sub-account, and
    the fence going down or up, move capital across the edge of the trading
    book: the card counts them with the bridge flows, at the USD in each
    row's note, and counts the events, not their legs."""
    tx = "2bbcfcaa7c11b312edf1a37cb95da12ecacfdb313b04fd5adb9a0f90f0898bf6"
    out = _display(tmp_path, [
        ("bridge_deposit", "bridge:job:2:a", "bridge", WUSDC, 4_985,
         _note(4.985, 1.0, 2)),
        # 1.5 XCH topped up at $5.20, and the send's fee: one flow, two legs.
        ("options_withdrawal", f"options_topup:{tx}", "options", "xch",
         -1_500_000_000_000, _options_note(-7.8, 5.2, "top-up 2bbcfcaa7c11b312")),
        ("options_withdrawal", f"options_topup:{tx}", "fee", "xch",
         -10_000_000, _options_note(-0.000052, 5.2, "top-up 2bbcfcaa7c11b312")),
        # The books began with the fence up: an event with nothing behind it.
        ("options_withdrawal", "options_fence_up:1", "options", "xch", 0,
         "options sub-account: fence up #1, nothing moved (where the books stand)"),
        # The fence went down holding 0.4 XCH and 2 wUSDC.b: one flow.
        ("options_deposit", "options_fence_down:2", "options", "xch",
         400_000_000_000, _options_note(2.08, 5.2, "fence down #2")),
        ("options_deposit", "options_fence_down:2", "options", WUSDC,
         2_000, _options_note(2.0, 1.0, "fence down #2")),
        ("fill", "trade:1", "base", WUSDC, 100, ""),
    ])
    assert out["bridge_flows"] == 1
    assert out["options_flows"] == 2
    assert out["net_deposits_usd"] == pytest.approx(
        4.985 - 7.8 - 0.000052 + 2.08 + 2.0)


def test_the_card_names_both_kinds_of_flow():
    from gui.widgets.main_window import MainWindow

    class _Card:
        _pnl_display: dict = {}
        _net_deposits_payload = MainWindow._net_deposits_payload

    card = _Card()
    assert card._net_deposits_payload()["secondary_text"] == "loading from ledger…"
    card._pnl_display = {"net_deposits_usd": 0.0, "bridge_flows": 0, "options_flows": 0}
    assert card._net_deposits_payload()["secondary_text"] == "no external flows yet"
    card._pnl_display = {"net_deposits_usd": 415.0, "bridge_flows": 3}
    assert (card._net_deposits_payload()["secondary_text"]
            == "external capital · 3 bridge flow(s)")
    card._pnl_display = {"net_deposits_usd": -5.72, "bridge_flows": 1, "options_flows": 2}
    payload = card._net_deposits_payload()
    assert payload["secondary_text"] == (
        "external capital · 1 bridge flow(s), 2 options flow(s)")
    assert payload["display_text"] == "-$5.72"
    card._pnl_display = {"net_deposits_usd": -7.8, "options_flows": 1}
    assert (card._net_deposits_payload()["secondary_text"]
            == "external capital · 1 options flow(s)")


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
    assert captured["options_flows"] == 0
    assert captured["net_deposits_usd"] == 0.0


@pytest.mark.parametrize("note,expected", [
    (_note(-2.16065, 3.95, 9), -2.16065),
    (_note(4.985, 1.0, 2), 4.985),
    # [OPTIONS-CAPITAL] An options leg carries the same field.
    ("options sub-account withdrawal; flow_usd=-7.8000000000; "
     "px_usd_per_unit=5.2000000000; ref=top-up 2bbcfcaa7c11b312", -7.8),
    ("options sub-account: fence up #1, nothing moved (where the books stand)",
     0.0),
    ("", 0.0),
    ("hand edit", 0.0),
    ("flow_usd=", 0.0),
    ("flow_usd=nan; x", 0.0),
    ("flow_usd=1e13", 0.0),          # the engine's magnitude bound
    (None, 0.0),
])
def test_the_note_parser_matches_the_engines(note, expected):
    assert _parse_bridge_flow_usd(note) == pytest.approx(expected)
