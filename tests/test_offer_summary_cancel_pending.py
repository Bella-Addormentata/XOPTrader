"""[S14 2026-09-13] 'cancel_pending' rows in the offer_log aggregates the GUI reads.

A submitted cancel still locks its coins and can still be taken until the
wallet reports it terminal.  So the Orders summary counts it on its own,
Locked includes its size, and the Balances tab's Offered / Deployed figures
include it.  Both queries run for real against a temporary SQLite file.
"""

from __future__ import annotations

import os
import sqlite3
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402


@pytest.fixture(scope="module")
def app():
    yield QApplication.instance() or QApplication(sys.argv)


def _seed(path: str) -> None:
    con = sqlite3.connect(path)
    con.execute("""CREATE TABLE offer_log (offer_id TEXT, pair_name TEXT, side TEXT,
                   price_mojos INTEGER, size_mojos INTEGER, status TEXT,
                   created_block INTEGER)""")
    con.executemany("INSERT INTO offer_log VALUES (?,?,?,?,?,?,?)", [
        ("0xa", "XCH/DBX", "ask", 1_000_000_000_000, 2_000_000_000_000, "pending", 1),
        ("0xb", "XCH/DBX", "ask", 1_000_000_000_000, 3_000_000_000_000, "cancel_pending", 2),
        ("0xc", "XCH/DBX", "ask", 1_000_000_000_000, 7_000_000_000_000, "cancelled", 3),
        ("0xd", "XCH/DBX", "ask", 1_000_000_000_000, 11_000_000_000_000, "filled", 4),
    ])
    con.commit()
    con.close()


def test_summary_counts_cancel_pending_and_locks_its_size(app, tmp_path):
    from gui.services.database_service import _DatabaseWorker

    db = tmp_path / "summary.db"
    _seed(str(db))
    worker = _DatabaseWorker()
    worker.open(str(db))
    captured: dict = {}
    worker.offer_summary_ready.connect(lambda d: captured.update(d))
    worker.fetch_offer_summary()

    assert captured["pending"] == 1
    assert captured["cancel_pending"] == 1
    assert captured["locked_mojos"] == 5_000_000_000_000, (
        "Locked must include the cancel_pending offer's size"
    )


def test_deployed_capital_includes_cancel_pending(app, tmp_path):
    from gui.services.database_service import _DatabaseWorker

    db = tmp_path / "deployed.db"
    _seed(str(db))
    worker = _DatabaseWorker()
    worker.open(str(db))
    captured: dict = {}
    worker.deployed_ready.connect(lambda d: captured.update(d))
    worker.fetch_deployed_capital()

    assert captured["offer_counts"] == {"XCH": 2}
    assert captured["pending_offers"] == 2
    assert captured["offered_units"]["XCH"] == pytest.approx(5.0)
