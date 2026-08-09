"""Offscreen tests for the live Warp Bridge widget.

The widget is fed exclusively by ``data["warp"]`` (the ``WarpService`` snapshot)
and must survive all three snapshot shapes it can receive:

* ``{}``                              -- before the first worker tick;
* ``{"enabled": ..., "error": ...}``  -- disabled or blocked (engine not built);
* the full built snapshot             -- ``hot_wallet``/``jobs``/``receiver_address``.

These tests also pin the two defects the rewrite fixed:

1. the copy said bridging was from **Ethereum** (it is **Base** — wUSDC.b is the
   Base-bridged CAT), and
2. the old widget read a ``receive_address`` key that does not exist in
   ``wallet_balances``, so its address never populated; the destination now comes
   from the snapshot's ``receiver_address``.

Runs headless (``QT_QPA_PLATFORM=offscreen``) under pytest or directly:

    .venv/Scripts/python.exe tests/test_warp_widget.py
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

# Must be set before PySide6 imports a platform plugin.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication, QLabel  # noqa: E402

from gui.widgets import warp as warp_mod  # noqa: E402
from gui.widgets.warp import WarpWidget, _eth, _short, _usdc, _wusdc  # noqa: E402


@pytest.fixture(scope="session")
def qapp():
    """A single offscreen QApplication for the whole session."""
    app = QApplication.instance() or QApplication([])
    yield app


# ---------------------------------------------------------------------------
# Snapshot fixtures -- shaped exactly like WarpEngine.snapshot()
# ---------------------------------------------------------------------------

_ASSET_ID = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"
_HOT_ADDR = "0xAbc0000000000000000000000000000000000001"
_XCH_ADDR = "xch1receiverexampleaddress00000000000000000000000000000000000wxyz"


def _hot(**over):
    h = {"address": _HOT_ADDR, "error": None, "eth_wei": 5_000_000_000_000_000,
         "usdc_micros": 1_500_000}
    h.update(over)
    return h


def _full(**over):
    snap = {
        "enabled": True,
        "dry_run": False,
        "auto_bridge": True,
        "network": "mainnet",
        "hot_wallet": _hot(),
        "active_job": None,
        "jobs": [],
        "min_micros": 100_000_000,     # 100 USDC
        "max_micros": 10_000_000_000,  # 10,000 USDC
        "receiver_address": _XCH_ADDR,
        "receiver_source": "config",
        "expected_asset_id": _ASSET_ID,
    }
    snap.update(over)
    return snap


def _job(**over):
    j = {
        "id": 1,
        "status": "COLLECTING_SIGS",
        "network": "mainnet",
        "created_at": "2026-08-07 10:00:00",
        "updated_at": "2026-08-07 10:05:00",
        "amount_usdc_micros": 2_000_000,
        "amount_mojos": 2_000,
        "post_tip_mojos": 1_994,
        "receiver_address": _XCH_ADDR,
        "bridge_tx_hash": "0x" + "ab" * 32,
        "bridge_nonce": "00" * 31 + "2a",
        "retry_count": 0,
        "last_error": None,
        "next_retry_at": None,
    }
    j.update(over)
    return j


def _make(qapp, snap=None):
    """Build a shown widget, optionally pre-fed with ``{"warp": snap}``."""
    w = WarpWidget()
    w.show()  # offscreen: makes isVisible() true so update_data renders
    if snap is not None:
        w.update_data({"warp": snap})
    return w


# ---------------------------------------------------------------------------
# Pure formatters (no Qt needed)
# ---------------------------------------------------------------------------

def test_unit_formatters():
    assert _usdc(1_500_000) == "1.50"
    assert _usdc(10_000_000_000) == "10,000.00"
    assert _usdc(None) == "—"          # SQL NULL / absent
    assert _wusdc(1_994) == "1.994"
    assert _wusdc(None) == "—"
    assert _eth(5_000_000_000_000_000) == "0.005000"
    assert _eth("garbage") == "—"
    assert _short("0x" + "ab" * 32).startswith("0xababab")
    assert "…" in _short("0x" + "ab" * 32)


# ---------------------------------------------------------------------------
# Snapshot-shape survival
# ---------------------------------------------------------------------------

def test_empty_snapshot_does_not_crash(qapp):
    w = _make(qapp, {})
    w.update_data({})            # no "warp" key at all
    w.update_data({"warp": None})
    assert "Connecting" in w._banner.text() or w._banner.text()


def test_disabled_snapshot_banner_and_controls(qapp):
    w = _make(qapp, {"enabled": False, "error": "warp disabled"})
    assert "disabled" in w._banner.text().lower()
    assert not w._bridge_btn.isEnabled()
    # No jobs -> table hidden, empty note shown.
    assert not w._jobs_table.isVisible()
    assert w._jobs_empty.isVisible()


def test_blocked_snapshot_surfaces_reason(qapp):
    w = _make(qapp, {"enabled": True, "error": "expected asset id mismatch"})
    text = w._banner.text().lower()
    assert "blocked" in text
    assert "mismatch" in w._banner.text()
    assert not w._bridge_btn.isEnabled()  # not built -> cannot bridge


# ---------------------------------------------------------------------------
# Full built snapshot
# ---------------------------------------------------------------------------

def test_built_snapshot_populates_addresses_and_balances(qapp):
    w = _make(qapp, _full())
    # Base deposit (hot-wallet) address and its copy button.
    assert w._addr_field.text() == _HOT_ADDR
    assert w._copy_btn.isEnabled()
    # Destination comes from the snapshot's receiver_address (defect #2 fix).
    assert w._dest_field.text() == _XCH_ADDR
    # Balances rendered; healthy gas -> no low-gas warning.
    assert "1.50" in w._usdc_lbl.text()
    assert "0.005000" in w._eth_lbl.text()
    assert not w._gas_lbl.isVisible()
    # Enabled + no active job -> Bridge now is actionable.
    assert w._bridge_btn.isEnabled()
    # Asset id shown and fully available via tooltip.
    assert w._asset_lbl.toolTip() == _ASSET_ID


def test_destination_ignores_wallet_balances(qapp):
    """The destination must come from warp.receiver_address, never wallet_balances."""
    w = _make(qapp)
    # A stray wallet_balances with a bogus address must not leak into the field.
    w.update_data({"warp": _full(), "wallet_balances": {"XCH": {"confirmed": 1.0}}})
    assert w._dest_field.text() == _XCH_ADDR


def test_low_gas_warning_appears(qapp):
    w = _make(qapp, _full(hot_wallet=_hot(eth_wei=1_000)))  # dust -> below threshold
    assert w._gas_lbl.isVisible()
    assert "gas" in w._gas_lbl.text().lower()


def test_hot_wallet_error_is_surfaced(qapp):
    w = _make(qapp, _full(hot_wallet={"address": _HOT_ADDR, "error": "rpc down"}))
    assert "unavailable" in w._usdc_lbl.text().lower()
    assert w._gas_lbl.isVisible()
    assert "rpc down" in w._gas_lbl.text()


def test_active_job_blocks_new_bridge(qapp):
    active = _job(status="BRIDGING")
    w = _make(qapp, _full(active_job=active, jobs=[active]))
    assert not w._bridge_btn.isEnabled()
    assert "in progress" in w._bridge_btn.toolTip().lower()


def test_failed_job_also_blocks_new_bridge(qapp):
    """A FAILED job keeps the single slot, so the button must stay disabled.

    The widget used to treat FAILED as terminal and enable the button, which
    ``request_bridge`` then rejected -- and the rejection was swallowed, so the
    control was silently dead.
    """
    failed = _job(status="FAILED", last_error="node rejected bundle")
    w = _make(qapp, _full(active_job=failed, jobs=[failed]))
    assert not w._bridge_btn.isEnabled()
    tip = w._bridge_btn.toolTip().lower()
    assert "retry" in tip and "sweep" in tip


def test_explorer_hosts_are_mainnet(qapp):
    w = _make(qapp, _full())
    assert w._basescan_url("0xdead").startswith("https://basescan.org/tx/0xdead")
    assert w._spacescan_url(_XCH_ADDR).startswith("https://spacescan.io/address/")


def test_dry_run_is_visibly_distinct_from_active(qapp):
    """Signing without broadcasting is safe, not healthy -- never show it green."""
    w = _make(qapp, _full(dry_run=True))
    assert "DRY RUN" in w._banner.text()
    assert "not broadcast" in w._banner.text()

    live = _make(qapp, _full(dry_run=False))
    assert "DRY RUN" not in live._banner.text()
    assert "active" in live._banner.text()


def test_dry_run_status_label_and_manual_hint(qapp):
    row = _job(status="DRY_RUN_OK", bridge_tx_hash=None)
    w = _make(qapp, _full(dry_run=True, jobs=[row]))
    assert w._jobs_table.item(0, 0).text() == "Dry run OK"
    # The caps hint must say manual ignores the floor but honours the cap.
    text = w._auto_lbl.text().lower()
    assert "bridge now" in text and "any balance" in text


def test_engine_failure_reads_as_blocked_not_disabled(qapp):
    """A failed startup anchor must not advise 'set warp.enabled: true'."""
    w = _make(qapp, {"enabled": True,
                     "error": "wrapped-asset anchor failed: derived ... != ..."})
    text = w._banner.text()
    assert "blocked" in text
    assert "disabled" not in text.lower()


def test_rejected_action_is_surfaced_in_the_banner(qapp):
    """Otherwise the worker swallows it and the click looks like a no-op."""
    w = _make(qapp, _full(action_error="a warp job is already active (job 3, FAILED)"))
    assert "Last action failed" in w._banner.text()
    assert "job 3" in w._banner.text()


def test_portal_and_docs_buttons_are_always_available(qapp):
    """A blocked operator is exactly who needs the out-of-band recovery link."""
    w = _make(qapp, {"enabled": True, "error": "engine blocked"})
    assert w._portal_btn.isEnabled()
    assert w._docs_btn.isEnabled()
    assert "warp.green" in w._portal_btn.toolTip()
    assert w._docs_btn.toolTip().startswith("https://docs.warp.green")


def test_wallet_derived_destination_is_labelled(qapp):
    w = _make(qapp, _full(receiver_address=_XCH_ADDR, receiver_source="wallet"))
    assert w._dest_field.text() == _XCH_ADDR
    assert "bot's own Chia wallet" in w._dest_field.toolTip()


# ---------------------------------------------------------------------------
# Jobs table
# ---------------------------------------------------------------------------

def test_jobs_table_populates_with_status_and_error(qapp):
    jobs = [
        _job(id=1, status="COMPLETED"),
        _job(id=2, status="FAILED", last_error="node rejected bundle"),
    ]
    w = _make(qapp, _full(jobs=jobs))
    table = w._jobs_table
    assert table.isVisible()
    assert not w._jobs_empty.isVisible()
    assert table.rowCount() == 2
    # Human-readable status label, amounts, and the error tooltip.
    assert table.item(0, 0).text() == "Completed"
    assert table.item(0, 1).text() == "2.00"          # 2_000_000 micros
    assert table.item(0, 2).text() == "1.994"         # post-tip mojos / 1000
    assert table.item(1, 0).text() == "Failed"
    assert table.item(1, 0).toolTip() == "node rejected bundle"


def test_explorer_urls_mainnet(qapp):
    w = _make(qapp, _full())
    assert w._basescan_url("ab" * 32) == "https://basescan.org/tx/0x" + "ab" * 32
    assert w._basescan_url("0xfeed") == "https://basescan.org/tx/0xfeed"
    assert w._spacescan_url(_XCH_ADDR) == f"https://spacescan.io/address/{_XCH_ADDR}"


# ---------------------------------------------------------------------------
# Action signals
# ---------------------------------------------------------------------------

def test_bridge_now_emits_signal(qapp):
    w = _make(qapp, _full())
    fired: list[bool] = []
    w.bridge_now_requested.connect(lambda: fired.append(True))
    assert w._bridge_btn.isEnabled()
    w._bridge_btn.click()
    assert fired == [True]


def test_job_action_signal_shape(qapp):
    w = _make(qapp, _full(jobs=[_job(id=7, status="FAILED")]))
    got: list[tuple[int, str]] = []
    w.job_action_requested.connect(lambda jid, act: got.append((jid, act)))
    # Emitting directly mirrors what the context-menu actions do (exec() would
    # block a headless run), and pins the (int, str) contract the service wants.
    w.job_action_requested.emit(7, "retry")
    assert got == [(7, "retry")]


# ---------------------------------------------------------------------------
# Defect #1: the copy is Base, never Ethereum
# ---------------------------------------------------------------------------

def test_copy_says_base_not_ethereum(qapp):
    w = _make(qapp, _full())
    texts = " ".join(lbl.text() for lbl in w.findChildren(QLabel))
    assert "Base" in texts
    assert "Ethereum" not in texts
    # Guard the module too: no lingering Ethereum-direction copy in source.
    assert "Ethereum" not in Path(warp_mod.__file__).read_text(encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))


# ---------------------------------------------------------------------------
# Unwrap controls
# ---------------------------------------------------------------------------

def test_unwrap_click_emits_mojos_and_destination(qapp):
    w = _make(qapp, {"enabled": True})
    got = []
    w.unwrap_requested.connect(lambda mojos, dest: got.append((mojos, dest)))

    w._unwrap_amount.setText("5.000")
    w._unwrap_dest.setText("0x" + "ab" * 20)
    w._unwrap_btn.click()

    assert got == [(5000, "0x" + "ab" * 20)]


def test_unwrap_click_refuses_a_non_numeric_amount(qapp):
    w = _make(qapp, {"enabled": True})
    got = []
    w.unwrap_requested.connect(lambda *a: got.append(a))

    w._unwrap_amount.setText("five")
    w._unwrap_dest.setText("0x" + "ab" * 20)
    w._unwrap_btn.click()

    assert got == [], "an unparseable amount must emit nothing"


def test_unwrap_destination_has_no_default(qapp):
    """The receiver is curried into the burn puzzle; a defaulted destination
    would make a mis-click unrecoverable by anyone."""
    w = _make(qapp, {"enabled": True})
    assert w._unwrap_dest.text() == ""


def test_outbound_statuses_have_labels(qapp):
    from gui.widgets import warp as warp_widget_mod

    for status in ("UNWRAP_CHECKS", "BURN_SENT", "BURNING",
                   "COLLECTING_EVM_SIGS", "RELAYING"):
        assert status in warp_widget_mod._STATUS_LABELS


def test_unwrap_amount_parser_never_rounds_or_crashes(qapp):
    """[PR72-REVIEW] float(text)*1000 rounded 0.0006 UP to 1 mojo (an unwrap
    the operator never typed), drifted on 5.0005, and raised an uncaught
    OverflowError on 1e309. Decimal + exact-increment + minimum, or no emit."""
    w = _make(qapp, {"enabled": True})
    got = []
    w.unwrap_requested.connect(lambda *a: got.append(a))
    w._unwrap_dest.setText("0x" + "ab" * 20)

    for bad in ("0.0006", "0.0004", "5.0005", "1e309", "nan", "inf", "-1", "0"):
        w._unwrap_amount.setText(bad)
        w._unwrap_btn.click()
    assert got == [], f"none of the malformed inputs may emit, got {got}"

    w._unwrap_amount.setText("0.001")
    w._unwrap_btn.click()
    w._unwrap_amount.setText("5.001")
    w._unwrap_btn.click()
    assert got == [(1, "0x" + "ab" * 20), (5001, "0x" + "ab" * 20)]
