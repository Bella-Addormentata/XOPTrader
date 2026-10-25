"""[WARP-LOST-SEND 2026-10-25] The engine leaves warp's Chia sends alone, as a
source scan.

LINT-CLASS GUARD, disclosed as such.  These tests read cpp/src/engine.cpp as
TEXT.  Engine is not constructible in xop_tests, so they pin the CALL-SITE
wiring no gtest reaches; the decisions are pinned in gtest
(cpp/tests/test_warp_send_guard.cpp).  A pass says Step 8 and the startup
prune still consult the guard where they must; it says nothing about
runtime behaviour.

What they guard: on 2026-10-24 Step 8's force-delete, armed by 12
heartbeats of pending_change on the XCH wallet, deleted every unconfirmed
transaction in it -- including the warp claim's funding send that WAS the
pending_change.  The job then waited two hours for a coin that could never
appear.  Every wallet-wide delete in the engine must now skip a wallet a
warp job has a Chia send in flight on, and that send's pending_change must
not count toward the escalation.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "cpp" / "src" / "engine.cpp"
ENGINE_HPP = REPO / "cpp" / "include" / "xop" / "engine.hpp"


def _engine() -> str:
    text = ENGINE.read_text(encoding="utf-8").replace("\r\n", "\n")
    # Drop // comments so commented-out code cannot satisfy a scan.
    return "\n".join(re.sub(r"//.*$", "", ln) for ln in text.split("\n"))


def test_the_pending_change_block_consults_the_guard_once_per_wallet():
    text = _engine()
    start = text.index("wallet_holds_warp_send(sb.wid, block_height)")
    block = text[start:start + 6000]
    # Not counted toward the escalation, not pruned, not force-deleted.
    assert "if (!warp_send_here) {\n" in block
    assert "pending_wallets_this_block.insert(sb.wid);" in block
    assert "&& !warp_send_here) {" in block                # the increment
    assert "!step8_pair_filter_ && !warp_send_here" in block  # the periodic prune
    assert text.count("!warp_send_here") == 3


def test_the_force_delete_iterates_only_unprotected_wallets():
    text = _engine()
    insert = text.index("pending_wallets_this_block.insert(sb.wid);")
    guard = text.rindex("if (!warp_send_here) {", 0, insert)
    assert insert - guard < 120, "the insert is inside the guard"
    loop = text.index("for (auto pw : pending_wallets_this_block)")
    assert loop > insert


def test_every_destructive_rpc_re_reads_the_guard_first():
    """[review] The GUI reserves a send before its RPC; the engine asks
    again, fresh, immediately before the prune and the force-delete."""
    text = _engine()
    prune = text.index("offer_mgr_->prune_stuck_transactions(\n"
                       "                                            {sb.wid}")
    fresh_call = text.rindex("wallet_holds_warp_send(sb.wid, block_height,", 0, prune)
    assert prune - fresh_call < 900, "the fresh re-read precedes the prune"
    assert "/*fresh=*/true" in text[fresh_call:prune]
    delete = text.index("delete_unconfirmed_transactions(pw);")
    fresh = text.rindex("wallet_holds_warp_send(pw, block_height,", 0, delete)
    assert delete - fresh < 900
    assert "continue;" in text[fresh:delete]
    assert "void Engine::refresh_warp_send_guard(BlockHeight block_height, bool fresh)" in text
    # [review] A headless engine (warp enabled, no database ever) regains its
    # cleanup after a bounded grace instead of failing closed for good.
    assert "kWarpDbMissingGraceHeartbeats" in text
    assert "warp_db_missing_heartbeats_ < kWarpDbMissingGraceHeartbeats" in text
    assert "if (!fresh && warp_send_guard_read_" in text


def test_the_startup_prune_skips_protected_wallets():
    text = _engine()
    boot = text.index("std::vector<std::int64_t> prune_candidates;")
    scope = text.index("execution::stuck_prune_scope(", boot)
    between = text[boot:scope]
    assert "wallet_holds_warp_send(wid, 0)" in between
    assert "prune_candidates.swap(kept);" in between


def test_the_guard_is_read_from_warp_jobs_db_and_fails_closed():
    text = _engine()
    reader = text.index("execution::WarpSendGuard read_warp_send_guard(")
    body = text[reader:text.index("\n}\n", reader)]
    assert "SQLITE_OPEN_READONLY" in body
    assert "FROM warp_jobs WHERE active = 1" in body
    assert "FROM dexie_takes WHERE status IN ('submitting', 'submitted', " in body
    assert "'unknown')" in body
    assert body.count("guard.readable = false") >= 6
    assert "std::filesystem::exists(db_path, ec)" in body
    # An active job whose state is not a JSON object protects every wallet.
    assert "if (state.is_discarded() || !state.is_object()) {" in body
    # An unanswerable path and a dexie_takes prepare error other than the
    # table's absence both read as unreadable.
    assert "if (ec) {" in body
    assert '"no such table"' in body
    holds = text.index("bool Engine::wallet_holds_warp_send(")
    holds_body = text[holds:text.index("\n}\n", holds)]
    assert "if (!g.readable) return true;" in holds_body
    assert 'resolve_wallet_id("xch")' in holds_body
    refresh = text.index("void Engine::refresh_warp_send_guard(")
    refresh_body = text[refresh:text.index("\n}\n", refresh)]
    # [review] The guard reads the GUI's own file (warp.jobs_db), not the
    # accounting override, which may name a database no reservation is in.
    assert "read_warp_send_guard(guard_db" in refresh_body
    assert "acc.warp_jobs_db_path.empty()" in refresh_body


def test_the_header_declares_the_guard_beside_the_escalation_counter():
    hpp = ENGINE_HPP.read_text(encoding="utf-8")
    assert '#include "xop/execution/warp_send_guard.hpp"' in hpp
    counter = hpp.index("kForceDeletePendingBlocks{12}")
    assert "execution::WarpSendGuard warp_send_guard_{};" in hpp[counter:counter + 1500]
