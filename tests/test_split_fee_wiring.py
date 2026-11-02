"""[SPLIT-FEE / STUCK-TX-COUNT 2026-11-02] Coin splits that never confirmed.

On 2026-11-01 no coin split confirmed for nine hours: the coin pool split the
same coins six to thirteen times each, and every time the Step 8 force-delete
wiped the split, unconfirmed, about a minute later (51 force-deletes).  Three
things together:

* the force-delete counter counted once per PAIR, not once per heartbeat, so
  four pairs on the XCH wallet reached kForceDeletePendingBlocks (~10 min) in
  three heartbeats;
* splits paid no fee;
* split_coins was re-sent after a timeout (one CAT split went out four times).

These are source-level pins: the behaviour lives inside two long coroutines
that need a wallet to run.
"""

from __future__ import annotations

from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "cpp" / "src" / "engine.cpp"
COIN_MANAGER = REPO / "cpp" / "src" / "execution" / "coin_manager.cpp"
COIN_MANAGER_HPP = REPO / "cpp" / "include" / "xop" / "execution" / "coin_manager.hpp"
RETRY_POLICY_HPP = REPO / "cpp" / "include" / "xop" / "rpc" / "rpc_retry_policy.hpp"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def _code(text: str) -> str:
    """Drop `// ...` tails so a comment cannot satisfy a scan."""
    return "\n".join(line.split("//", 1)[0] for line in text.split("\n"))


def _function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def _manage_offers() -> str:
    return _code(_function_body(
        _read(ENGINE), "asio::awaitable<void> Engine::step_manage_offers("))


def _coin_pool() -> str:
    return _code(_function_body(
        _read(ENGINE), "asio::awaitable<void> Engine::step_maintain_coin_pool("))


def test_pending_change_is_counted_per_wallet_once_per_step_8():
    """One counter for every wallet, bumped once per PAIR, reached the
    force-delete in ~1 minute instead of ~10.  The streak is per wallet, counted
    at the wallet's first sighting in each Step 8."""
    body = _manage_offers()
    assert "consecutive_pending_blocks_" not in body
    assert "saw_pending_change" not in body
    first = body.index("pending_wallets_this_block.insert(sb.wid).second;")
    bump = body.index("pending_streak = ++pending_streak_by_wallet_[sb.wid];", first)
    assert "if (first_sighting && !step8_pair_filter_) {" in body[first:bump]
    # A wallet that was not pending in this Step 8 starts over.
    assert ("std::erase_if(pending_streak_by_wallet_, [&](const auto& kv) {\n"
            "        return pending_wallets_this_block.count(kv.first) == 0;") in body
    hpp = _code(_read(REPO / "cpp" / "include" / "xop" / "engine.hpp"))
    assert "std::map<std::int64_t, std::uint32_t> pending_streak_by_wallet_;" in hpp
    assert "consecutive_pending_blocks_" not in hpp


def test_a_wallet_seen_clear_starts_over():
    """[review #214] A spend submitted on a clean wallet earlier in the same
    heartbeat (an earlier pair's secure cancel, a coin split) must not inherit
    a stuck streak and be force-deleted as one."""
    body = _manage_offers()
    fire = body.index("&& pending_streak >= kForceDeletePendingBlocks) {")
    reset = body.index("pending_streak_by_wallet_.erase(pw);", fire)
    clear = body.index("} else {", reset)
    assert body[reset:clear].count("}") == 1, "the else of `if (pending > 0)`"
    assert "pending_streak_by_wallet_.erase(sb.wid);" in body[clear:clear + 200]
    pool = _coin_pool()
    assert "pending_streak_by_wallet_.erase(1);" in pool
    assert "pending_streak_by_wallet_.erase(info.wallet_id);" in pool


def test_coin_pool_splits_pay_the_cancel_fee_of_their_asset():
    pool = _coin_pool()
    assert "constexpr Mojo split_fee = 0;" not in pool
    xch = pool.index("strategy::fee::ActionClass::CancelXch")
    cat = pool.index("strategy::fee::ActionClass::CancelCat")
    xch_split = pool.index("ensure_split(\n                    1, xch_target_count, "
                           "target_mojos, split_fee)")
    cat_split = pool.index("ensure_split(\n                    info.wallet_id, pool_count, "
                           "target_mojos, split_fee)")
    assert xch < xch_split < cat < cat_split
    # A split that was sent is charged to the daily fee budget, once per site.
    assert pool.count("fee_tracker_->record_fee(") == 2
    # The tracker's "budget exhausted" skips the split rather than sending it
    # for nothing.  [review #214] The budget is asked directly too: with the
    # fee controller on, a cancel class returns its full fee when it is spent.
    assert ("if (fee_tracker_->enabled()\n"
            "            && (fee == 0 || !fee_tracker_->is_within_budget(block_height, fee))) {\n"
            "            return std::nullopt;") in pool
    assert pool.count("split skipped -- the fee") == 2


def test_startup_splits_book_their_fee_at_a_real_height():
    """[review #214] A fee booked at height 0 is pruned from the daily budget at
    the first real query: startup asks for the height, and skips without one."""
    engine = _code(_read(ENGINE))
    assert "step_maintain_coin_pool(0)" not in engine
    boot = engine.index("BlockHeight boot_height = 0;")
    call = engine.index("co_await step_maintain_coin_pool(boot_height);", boot)
    between = engine[boot:call]
    assert "co_await full_node_->get_block_height()" in between
    assert "if (boot_height > 0) {" in between


def test_the_force_delete_wipes_only_the_wallet_whose_streak_ran_out():
    """[review #214] Each wallet escalates on its own streak: no shared counter
    for one busy wallet to reset, no sweep of wallets other pairs spent on in
    this Step 8.  The transport guard still returns before the reset, and the
    ForceDelete signal fires once per Step 8."""
    body = _manage_offers()
    fire = body.index("&& pending_streak >= kForceDeletePendingBlocks) {")
    wipe = body.index("delete_unconfirmed_transactions(pw)", fire)
    between = body[fire:wipe]
    assert "const std::int64_t pw = sb.wid;" in between
    assert "for (auto pw : pending_wallets_this_block)" not in body
    assert "co_return;" in between
    reset = body.index("pending_streak_by_wallet_.erase(pw);", wipe)
    tail = body[wipe:reset]
    assert "if (!force_delete_signalled) {" in tail
    assert "strategy::fee::Signal::ForceDelete" in tail
