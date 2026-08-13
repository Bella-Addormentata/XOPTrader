"""The Target Portfolio Allocation panel must not present a confident
percentage split computed over only *some* of the held assets.

Field failure (2026-08-12): the engine could not reach the Chia wallet RPC,
so it published no mid prices. Only wUSDC.b resolved (hard $1 stablecoin
anchor), every other asset priced 0 and was dropped from the maths, and the
panel reported "100% wUSDC.B" while the operator actually held XCH, BYC and
DBX. The number was arithmetically true over the priced subset and totally
false as an allocation -- the worst kind of display bug, because it looks
authoritative."""

from __future__ import annotations

import os

import pytest

pytest.importorskip("PySide6")

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication  # noqa: E402

from gui.widgets.wallet_balances import WalletBalancesWidget  # noqa: E402


@pytest.fixture(scope="module")
def qapp():
    return QApplication.instance() or QApplication([])


def _widget(monkeypatch, qapp, *, prices: dict, holdings: dict):
    """A panel whose price feed and wallet->symbol mapping are pinned, so the
    test exercises the DISPLAY logic rather than the market-data plumbing."""
    w = WalletBalancesWidget()
    w._last_pairs_cfg = [
        {"name": "XCH/wUSDC.b", "enabled": True},
        {"name": "XCH/BYC", "enabled": True},
    ]
    w._last_balances = {
        name: {"wallet_type": 6, "asset_id": "", "confirmed": amount}
        for name, amount in holdings.items()
    }
    monkeypatch.setattr(w, "_asset_prices_usdc", lambda: dict(prices))
    monkeypatch.setattr(
        w, "_wallet_asset_symbol",
        lambda wallet_name, *a, **k: wallet_name,   # wallet name IS the symbol
    )
    return w


def test_partial_pricing_warns_instead_of_implying_a_full_allocation(
    monkeypatch, qapp
):
    w = _widget(
        monkeypatch, qapp,
        prices={"WUSDC.B": 1.0},                    # XCH/BYC unpriced
        holdings={"WUSDC.B": 36.5, "XCH": 80.0, "BYC": 64.0},
    )
    w._refresh_allocation_table()

    shown = {
        w._alloc_table.item(r, 0).text(): w._alloc_table.item(r, 1).text()
        for r in range(w._alloc_table.rowCount())
    }
    # The arithmetic is unchanged -- the priced asset still reads 100% --
    # but it must no longer stand unqualified.
    assert shown["WUSDC.B"] == "100.00%"
    hint = w._alloc_hint_label.text()
    assert "only priced assets" in hint, f"no partial-pricing warning: {hint!r}"
    for missing in ("XCH", "BYC"):
        assert missing in hint, f"{missing} should be named as unpriced"


def test_no_warning_once_everything_is_priced(monkeypatch, qapp):
    w = _widget(
        monkeypatch, qapp,
        prices={"WUSDC.B": 1.0, "XCH": 1.30, "BYC": 1.0},
        holdings={"WUSDC.B": 50.0, "XCH": 50.0, "BYC": 50.0},
    )
    w._refresh_allocation_table()

    hint = w._alloc_hint_label.text()
    assert "only priced assets" not in hint, (
        f"fully-priced portfolio must not warn: {hint!r}"
    )
    shown = {
        w._alloc_table.item(r, 0).text(): w._alloc_table.item(r, 1).text()
        for r in range(w._alloc_table.rowCount())
    }
    assert shown["WUSDC.B"] != "100.00%", "three priced assets cannot be 100% one"


def test_no_prices_at_all_keeps_the_waiting_message(monkeypatch, qapp):
    w = _widget(
        monkeypatch, qapp,
        prices={},
        holdings={"WUSDC.B": 36.5, "XCH": 80.0},
    )
    w._refresh_allocation_table()
    assert "Waiting for market data" in w._alloc_hint_label.text()
