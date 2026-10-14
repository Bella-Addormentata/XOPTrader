"""Target Allocation editing must survive the periodic balance refresh, and
"Apply Suggested Values" must copy Suggested % into Target %.

Field report (2026-10-13): typing a new Target % often snapped back to the
old value before Enter could be pressed.  Every balance tick rebuilt the
table with setItem(), and Qt pushes a changed cell's model value straight
back into its open editor (QAbstractItemView.dataChanged -> setEditorData).
"""

from __future__ import annotations

import os

import pytest

pytest.importorskip("PySide6")

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import Qt  # noqa: E402
from PySide6.QtTest import QTest  # noqa: E402
from PySide6.QtWidgets import QApplication, QLineEdit  # noqa: E402

from gui.widgets.wallet_balances import WalletBalancesWidget  # noqa: E402


@pytest.fixture(scope="module")
def qapp():
    return QApplication.instance() or QApplication([])


@pytest.fixture
def widget(monkeypatch, qapp):
    w = WalletBalancesWidget()
    # showEvent would start the dexie-backed advisory worker.
    monkeypatch.setattr(w, "_refresh_suggested_allocation", lambda: None)
    monkeypatch.setattr(w, "_save_target_allocations", lambda: None)
    w._last_pairs_cfg = [{"name": "XCH/BYC", "enabled": True}]
    w._target_allocations = {"BYC": 50.0, "XCH": 50.0}
    w._refresh_allocation_table()
    w.show()
    yield w
    w.close()
    w.deleteLater()


def _row(w, asset: str) -> int:
    for r in range(w._alloc_table.rowCount()):
        if w._alloc_table.item(r, 0).text() == asset:
            return r
    raise AssertionError(asset)


def _open_editor(w, qapp, row: int, col: int) -> QLineEdit:
    idx = w._alloc_table.model().index(row, col)
    w._alloc_table.setCurrentIndex(idx)
    w._alloc_table.edit(idx)
    qapp.processEvents()
    editor = w._alloc_table.findChild(QLineEdit)
    assert editor is not None
    return editor


def test_refresh_does_not_revert_an_open_target_editor(widget, qapp):
    row = _row(widget, "XCH")
    editor = _open_editor(widget, qapp, row, 2)
    editor.setText("70")

    widget._refresh_allocation_table()      # a balance tick mid-typing
    qapp.processEvents()

    assert editor.text() == "70"
    assert widget._alloc_refresh_deferred


def test_committed_edit_sticks_and_deferred_refresh_runs(widget, qapp):
    row = _row(widget, "XCH")
    editor = _open_editor(widget, qapp, row, 2)
    editor.setText("70")
    widget._refresh_allocation_table()

    QTest.keyClick(editor, Qt.Key.Key_Return)
    for _ in range(50):
        if not widget._alloc_refresh_deferred:
            break
        QTest.qWait(20)

    assert widget._target_allocations["XCH"] == 70.0
    assert not widget._alloc_refresh_deferred
    assert widget._alloc_table.item(_row(widget, "XCH"), 2).text() == "70.00"
    assert "120.00%" in widget._alloc_sum_label.text()


def test_apply_suggested_values_copies_suggested_into_targets(widget):
    assert not widget._apply_suggested_btn.isEnabled()   # still pending

    widget._on_suggested_ready({
        "assets": {"XCH": {"suggested_pct": 62.5}},
        "marginal_note": "",
        "computed_at": "now",
    })
    assert widget._apply_suggested_btn.isEnabled()

    widget._apply_suggested_btn.click()

    assert widget._target_allocations["XCH"] == 62.5
    assert widget._target_allocations["BYC"] == 50.0   # no suggestion: kept
    assert widget._alloc_table.item(_row(widget, "XCH"), 2).text() == "62.50"
