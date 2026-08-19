"""The Base Wallet receive-QR: payload, rendering, and the dialog.

The QR exists so an exchange app's withdraw screen (e.g. Coinbase) can be
pointed at the hot wallet without hand-typing a 42-character address.  Two
properties are safety-relevant and pinned here:

* the QR encodes the PLAIN checksummed address, byte-for-byte -- a URI
  scheme or any transformation risks an exchange scanner mis-parsing the
  destination of real money;
* the dialog carries the Base-network warning, because the address is
  valid on every EVM chain and this wallet's tooling only operates on Base.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication, QLabel  # noqa: E402

from gui.widgets.base_wallet import (  # noqa: E402
    BaseWalletWidget,
    qr_payload,
    qr_png,
)

_ADDR = "0xAbC0000000000000000000000000000000000001"


@pytest.fixture(scope="session")
def qapp():
    app = QApplication.instance() or QApplication([])
    yield app


def test_payload_is_the_plain_address_verbatim():
    """No URI scheme, no chain suffix, no case change: exchange scanners
    must receive exactly the checksummed address."""
    assert qr_payload(_ADDR) == _ADDR
    assert qr_payload(f"  {_ADDR}  ") == _ADDR  # whitespace-trimmed only


def test_qr_png_is_a_valid_png_and_deterministic():
    data = qr_png(_ADDR)
    assert data[:8] == bytes.fromhex("89504e470d0a1a0a"), "PNG magic"
    assert len(data) > 200, "a real QR, not a stub"
    assert qr_png(_ADDR) == data, "same address, same code"
    # A different address must yield a different code (the pixmap is not
    # accidentally cached or hardcoded).
    assert qr_png("0x" + "12" * 20) != data


def test_dialog_carries_the_code_the_address_and_the_warning(qapp):
    w = BaseWalletWidget()
    dlg = w.build_qr_dialog(_ADDR)
    try:
        img = dlg.findChild(QLabel, "qr_image")
        assert img is not None and not img.pixmap().isNull()

        addr = dlg.findChild(QLabel, "qr_address")
        assert addr is not None and addr.text() == _ADDR

        warn = dlg.findChild(QLabel, "qr_warning")
        assert warn is not None
        assert "Base" in warn.text(), (
            "the network warning is the QR's safety companion: the address "
            "is valid on every EVM chain, and only Base is supported here"
        )
    finally:
        dlg.deleteLater()
        w.deleteLater()


def test_convert_wrap_floors_and_emits_the_exact_payload(qapp, monkeypatch):
    """The GUI floor and the emitted payload are the safety-critical pair:
    a wrong floor either bounces the operator (contract revert) or wraps
    a different amount than the dialog showed."""
    from PySide6.QtWidgets import QMessageBox

    w = BaseWalletWidget()
    prompts = []
    monkeypatch.setattr(
        QMessageBox, "question",
        staticmethod(lambda *a, **k: prompts.append(a[2]) or
                     QMessageBox.StandardButton.Yes),
    )
    emitted = []
    w.wallet_action_requested.connect(lambda a, p: emitted.append((a, p)))

    # 0.0049000000005 ETH has a sub-granule tail; the floor must trim it.
    w._convert_dir.setCurrentIndex(0)
    w._convert_amount.setText("0.0049000000005")
    w._on_convert_clicked()

    assert emitted, "confirmed wrap must emit the action"
    action, payload = emitted[-1]
    assert action == "wrap_eth"
    wei = payload["amount_wei"]
    assert wei == 4_900_000_000_000_000, "floored to the 1e12 granularity"
    assert wei % 10 ** 12 == 0
    # The dialog showed EXACTLY the floored amount, both denominations.
    assert "0.0049" in prompts[-1]
    assert "4.9" in prompts[-1], "milliETH out shown exactly"
    w.deleteLater()


def test_convert_unwrap_emits_exact_units(qapp, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    w = BaseWalletWidget()
    prompts = []
    monkeypatch.setattr(
        QMessageBox, "question",
        staticmethod(lambda *a, **k: prompts.append(a[2]) or
                     QMessageBox.StandardButton.Yes),
    )
    emitted = []
    w.wallet_action_requested.connect(lambda a, p: emitted.append((a, p)))

    w._convert_dir.setCurrentIndex(1)
    w._convert_amount.setText("2.5")
    w._on_convert_clicked()

    action, payload = emitted[-1]
    assert action == "unwrap_millieth"
    assert payload["amount_units"] == 2_500
    assert "2.5" in prompts[-1] and "0.0025" in prompts[-1]
    w.deleteLater()


def test_convert_declined_emits_nothing(qapp, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    w = BaseWalletWidget()
    monkeypatch.setattr(
        QMessageBox, "question",
        staticmethod(lambda *a, **k: QMessageBox.StandardButton.No),
    )
    emitted = []
    w.wallet_action_requested.connect(lambda a, p: emitted.append((a, p)))
    w._convert_dir.setCurrentIndex(0)
    w._convert_amount.setText("0.001")
    w._on_convert_clicked()
    assert emitted == [], "No must mean no"
    w.deleteLater()


def test_qr_button_disabled_without_an_address(qapp):
    """An unconfigured wallet has nothing to encode; the button greys out
    with Copy instead of silently no-opping."""
    w = BaseWalletWidget()
    w.update_data({"warp": {"base_wallet": {"configured": False}}})
    w._render()
    assert not w._qr_btn.isEnabled()
    w.update_data({"warp": {"base_wallet": {
        "configured": True, "address": _ADDR,
        "eth_wei": 10 ** 15, "usdc_micros": 0, "millieth_units": 0,
    }}})
    w._render()
    assert w._qr_btn.isEnabled()
    w.deleteLater()


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
