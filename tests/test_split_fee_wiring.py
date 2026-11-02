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


def test_the_pending_change_counter_counts_once_per_step_8():
    body = _manage_offers()
    flag = "bool saw_pending_change = false;"
    assert body.count(flag) == 1, "one flag for the whole Step 8"
    pending = body.index("std::set<std::int64_t> pending_wallets_this_block;")
    loop = body.index("for (auto& [pair_name, pcs] : cycle_)", pending)
    at = body.index(flag)
    assert pending < at < loop, (
        "declared inside the pair loop the counter counts once per PAIR and "
        "the force-delete fires after ~1 minute instead of ~10")
    # The increment is still guarded by it, and the reset is still per run.
    bump = body.index("++consecutive_pending_blocks_;")
    guard = body.rindex("if (!saw_pending_change", 0, bump)
    assert bump - guard < 200
    assert "saw_pending_change = true;" in body[bump:bump + 120]
    assert "if (pending_wallets_this_block.empty()) {" in body


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


def test_a_fired_force_delete_reaches_every_pending_wallet_of_the_step():
    """[review #214] Counted once per Step 8 the escalation fires at the first
    pending pair; it stays on for the rest of the step so later pairs' wallets
    are wiped too, each once, and the counter resets once."""
    body = _manage_offers()
    pending = body.index("std::set<std::int64_t> pending_wallets_this_block;")
    loop = body.index("for (auto& [pair_name, pcs] : cycle_)", pending)
    assert pending < body.index("bool force_delete_this_step = false;") < loop
    assert pending < body.index("std::set<std::int64_t> force_deleted_wallets;") < loop
    fire = body.index("&& (force_delete_this_step\n")
    wipe = body.index("delete_unconfirmed_transactions(pw)", fire)
    between = body[fire:wipe]
    assert "if (force_deleted_wallets.count(pw) > 0) continue;" in between
    assert "force_deleted_wallets.insert(pw);" in between
    # The transport guard still returns before the reset can run.
    assert between.index("co_return;") < between.index("force_deleted_wallets.insert(pw);")
    reset = body.index("consecutive_pending_blocks_ = 0;", wipe)
    tail = body[wipe:reset + 120]
    assert "if (!force_delete_this_step) {" in tail
    assert "force_delete_this_step = true;" in tail


def test_a_cat_coin_carries_none_of_its_split_fee():
    hpp = _code(_read(COIN_MANAGER_HPP))
    assert "return wallet_id == 1 ? fee : 0;" in hpp
    body = _code(_function_body(
        _read(COIN_MANAGER), "asio::awaitable<SplitResult> CoinManager::ensure_split("))
    assert "const Mojo coin_fee = fee_carried_by_coin(wallet_id, fee);" in body
    # The planner and the balance check work in the coin's own mojos ...
    assert "plan_split_for_coin(c.amount, needed, target_amount_mojos, coin_fee)" in body
    assert "total_available < target_amount_mojos + coin_fee" in body
    assert "plan_split_for_coin(c.amount, needed, target_amount_mojos, fee)" not in body
    # ... and the wallet gets the whole fee.
    rpc = body.index("co_await wallet_->split_coins(")
    assert "fee);" in body[rpc:rpc + 300]


def test_split_coins_is_never_resent():
    hpp = _code(_read(RETRY_POLICY_HPP))
    assert 'endpoint == "split_coins"' in hpp
