"""[WARP-MILLIETH 2026-10-24] Secondary bridge-asset wiring in the C++ engine,
as a source scan.

LINT-CLASS GUARD, disclosed as such.  These tests read cpp/src/engine.cpp as
TEXT.  Engine is not constructible in xop_tests, so they pin the CALL-SITE
wiring no gtest reaches; every decision is pinned in gtest
(cpp/tests/test_bridge_ingest.cpp: plan_secondary_inventory,
window_rebase_verdict, ...).  A pass says the engine still calls those
decisions where it must; it says nothing about runtime behaviour.

What they guard: a wmilliETH.b warp flow is capital, not performance.  Its
booking must land before the ledger invariant ties the books, and every
place that moves the asset's quantity as a capital event -- the secondary
scan, its wallet-truth pass, and Step 11's restart reconcile (a flow whose
journal row survived a restart that the persisted inventory did not) -- must
re-anchor the drawdown peak and restart the rolling-loss window, or a
withdrawal reads as a trading loss.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "cpp" / "src" / "engine.cpp"

HEARTBEAT = "asio::awaitable<void> Engine::on_new_block_coro(BlockHeight block_height)"
SECONDARY = "asio::awaitable<void> Engine::step_ingest_secondary_bridge_flows("
STEP11 = "void Engine::step_update_pnl(BlockHeight block_height)"
STEP13 = "void Engine::step_check_alerts(BlockHeight block_height)"


def _engine() -> str:
    text = ENGINE.read_text(encoding="utf-8").replace("\r\n", "\n")
    # Drop // comments so commented-out code cannot satisfy a scan.
    return "\n".join(re.sub(r"//.*$", "", ln) for ln in text.split("\n"))


def _body(text: str, signature: str) -> str:
    start = text.index(signature)
    return text[start:text.index("\n}\n", start)]


def test_the_secondary_scan_runs_after_the_bridge_scan_and_before_the_invariant():
    beat = _body(_engine(), HEARTBEAT)
    primary = beat.index("co_await step_ingest_bridge_flows(block_height)")
    secondary = beat.index("co_await step_ingest_secondary_bridge_flows(block_height)")
    invariant = beat.index("step_check_ledger_invariant(block_height)")
    assert primary < secondary < invariant


def test_a_bridge_asset_with_no_cat_wallet_yet_opens_at_zero():
    """The first wrap creates the CAT wallet; without a zero opening taken
    while the wallet proved it held none, that deposit could never book."""
    text = _engine()
    branch = text.index("if (wid <= 0) {")
    body = text[branch:branch + 2500]
    proven = body.index("startup_wallet_synced && offer_mgr_->wallet_ids_resolved()")
    end = body.index("continue;", proven)
    assert "is_secondary_bridge_asset(" in body[proven:end]
    assert "genesis_balances[AssetId{aid}] = 0;" in body[proven:end]
    assert "genesis_observed_at[AssetId{aid}]" in body[proven:end]


def test_step11s_restart_reconcile_re_anchors_a_secondary_asset():
    """The one-shot reconcile is where a restart's stale restored quantity is
    corrected; for a secondary bridge asset that is a capital event."""
    step11 = _body(_engine(), STEP11)
    write = step11.index("note_inventory_absolute_write(aid);")
    tail = step11[write:write + 1500]
    assert "is_secondary_bridge_asset(" in tail
    assert "peak_equity_hwm_usd_ = 0.0;" in tail
    assert "pnl_window_rebase_assets_[aid] = block_height;" in tail


def test_the_secondary_scan_re_anchors_and_rebases_together():
    scan = _body(_engine(), SECONDARY)
    assert "peak_equity_hwm_usd_ = 0.0;" in scan
    assert "pnl_window_rebase_assets_[a] = block_height;" in scan


def test_wallet_truth_re_arms_step11_on_a_post_booking_snapshot():
    """One wallet-truth writer, with one set of rules: a flow whose quantity
    an earlier absolute write may contain re-arms Step 11's one-shot
    reconcile, which then waits for a snapshot from the booking on."""
    text = _engine()
    scan = _body(text, SECONDARY)
    assert "inventory_reconciled_assets_.erase(sid);" in scan
    assert "inventory_reconcile_not_before_[sid] = block_height;" in scan
    step11 = _body(text, STEP11)
    gate = step11.index("inventory_reconcile_not_before_.find(aid)")
    write = step11.index("inventory_->adjust_quantity(AssetId{aid}")
    assert gate < write
    assert "bal.as_of_block < nb->second" in step11[gate:write]


def test_step13_restarts_the_window_by_the_tested_verdict():
    step13 = _body(_engine(), STEP13)
    rebase = step13.index("pnl_window_rebase_assets_")
    loss = step13.index("window_start_pnl")
    assert rebase < loss, "the rebase must run before the window is judged"
    assert "accounting::window_rebase_verdict(" in step13[rebase:loss]
