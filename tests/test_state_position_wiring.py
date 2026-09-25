"""[SEED-FAIL-CLOSED 2026-09-22] State position wiring in the C++ engine, as a
source scan.

LINT-CLASS GUARD, disclosed as such.  These tests read cpp/src/engine.cpp as
TEXT.  They pin call-site WIRING that no C++ unit test reaches -- Engine is not
constructible in xop_tests -- while every DECISION is pinned by gtest
(cpp/tests/test_state_position_truth.cpp).  A pass here says the engine still
CALLS those decisions where it must; it says nothing about runtime behaviour.

What they guard.  State's positions are what the risk limits read.  On
2026-09-22 all three startup balance reads timed out and the seed skipped each
asset at DEBUG, so State began empty.  Fills then made its XCH and DBX
positions small but non-zero, and Step 8's recovery -- which fired only on an
exactly-zero position -- never fired.  The single-CAT cap read DBX as 52.8% and
then about 96% of the portfolio, against a wallet where it was about a fifth,
and sized the XCH/DBX ask to zero.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "cpp" / "src" / "engine.cpp"

STEP8 = "asio::awaitable<void> Engine::step_manage_offers(BlockHeight block_height)"
POLL_LOOP = "asio::awaitable<void> Engine::poll_loop_coro()"
HELPER = "Engine::reconcile_state_position("
BRIDGE_SCAN = "asio::awaitable<void> Engine::step_ingest_bridge_flows("
HEARTBEAT = "asio::awaitable<void> Engine::on_new_block_coro(BlockHeight block_height)"

# The defect's shape: something gated on a State position being zero.  The
# argument may be brace-initialised (`AssetId{x}`), so only `;` bounds it: a
# first version excluded braces too, and the mutation check found that it
# could not see `get_position(AssetId{sb.label}).balance == 0`.
ZERO_GUARD = re.compile(r"get_position\s*\([^;]*?\)\s*\.\s*balance\s*(?:==|<=)\s*0\b")
# An assignment to a cache slot (not a comparison).
CACHE_WRITE = re.compile(r"cached_wallet_balances_\s*\[\s*([^\]]+?)\s*\]\s*=(?!=)")
DELTA_WRITE = re.compile(r"state_->record_(?:buy|sell)\s*\(")


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def _strip_line_comments(text: str) -> str:
    """Drop `// ...` tails so commented-out code cannot satisfy a scan.  A `//`
    inside a string literal is left alone."""
    out = []
    for line in text.split("\n"):
        in_string = False
        i = 0
        cut = len(line)
        while i < len(line):
            ch = line[i]
            if in_string:
                if ch == "\\":
                    i += 2
                    continue
                if ch == '"':
                    in_string = False
            elif ch == '"':
                in_string = True
            elif ch == "/" and line[i:i + 2] == "//":
                cut = i
                break
            i += 1
        out.append(line[:cut])
    return "\n".join(out)


def _engine() -> str:
    return _strip_line_comments(_read(ENGINE))


def _function_body(text: str, signature: str) -> str:
    """The text of the function whose definition starts with `signature`, up to
    the closing brace at column 0."""
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def _matching(text: str, open_index: int) -> int:
    """Index of the bracket closing the one at `open_index`, skipping string
    and character literals.  Comments must already be stripped."""
    pairs = {"(": ")", "{": "}", "[": "]"}
    opener = text[open_index]
    closer = pairs[opener]
    depth = 0
    quote = ""
    i = open_index
    while i < len(text):
        ch = text[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = ""
        elif ch == '"' or (ch == "'" and not text[i - 1].isalnum()):
            quote = ch
        elif ch == opener:
            depth += 1
        elif ch == closer:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise AssertionError(f"unbalanced {opener!r} at offset {open_index}")


def _calls(text: str, callee: str) -> list[list[str]]:
    """The top-level-comma-split arguments of every `<callee>(` in `text`."""
    results: list[list[str]] = []
    for match in re.finditer(re.escape(callee) + r"\s*\(", text):
        open_index = match.end() - 1
        inner = text[open_index + 1:_matching(text, open_index)]
        args, depth, current = [], 0, ""
        for ch in inner:
            if ch in "({[":
                depth += 1
            elif ch in ")}]":
                depth -= 1
            if ch == "," and depth == 0:
                args.append(current.strip())
                current = ""
            else:
                current += ch
        args.append(current.strip())
        results.append(args)
    return results


def test_no_state_write_waits_for_an_exactly_zero_position() -> None:
    """The old recovery's guard, anywhere in the engine."""
    hits = ZERO_GUARD.findall(_engine())
    assert not hits, (
        "engine.cpp gates something on a State position being exactly zero "
        f"({hits}).  That guard is the 2026-09-22 defect: fills made the "
        "positions non-zero first and the recovery never fired."
    )


def test_step8_never_reads_state_positions() -> None:
    """Step 8 WRITES State from the wallet; nothing in it may decide by what
    State held, which is how a zero-only (or any other) guard would return."""
    body = _function_body(_engine(), STEP8)
    assert "get_position(" not in body
    assert "get_all_positions(" not in body


def test_every_step8_balance_read_reconciles_state() -> None:
    """Each validated balance Step 8 caches also becomes State's position, for
    the same asset, from the same confirmed value and under the same validation
    bit.  Three writers today: the verification pass, the empty-ladder
    liveness refresh (the only read a suspended pair's CAT gets) and the main
    loop's balance gate."""
    body = _function_body(_engine(), STEP8)
    writes = list(CACHE_WRITE.finditer(body))
    assert len(writes) >= 3, "expected the verification pass, the liveness refresh and the balance gate"

    flags = re.findall(r"const\s+bool\s+fields_validated\s*=\s*([^;]+);", body)
    assert len(flags) == len(writes)
    for flag in flags:
        # A reply missing either field is not a balance (S19 round 28).
        assert 'contains("confirmed_wallet_balance")' in flag
        assert 'contains("pending_change")' in flag
        assert "&&" in flag

    for i, write in enumerate(writes):
        key = write.group(1).strip()
        entry_end = body.index(";", write.end())
        entry = body[write.end():entry_end].strip()
        assert entry.startswith("{") and entry.endswith("}"), entry
        fields = [f.strip() for f in entry[1:-1].split(",")]
        stored_confirmed, stored_flag = fields[1], fields[-1]

        window_end = writes[i + 1].start() if i + 1 < len(writes) else len(body)
        calls = _calls(body[write.end():window_end], "reconcile_state_position")
        assert calls, f"the cache write for {key} is not followed by a State reconcile"
        args = calls[0]
        assert args == [key, stored_confirmed, stored_flag, "block_height"], (
            f"cache write for {key} reconciles with {args}; expected the same "
            f"asset, the confirmed balance it stored ({stored_confirmed}) and "
            f"the flag stored with it ({stored_flag})"
        )
        assert stored_flag == "fields_validated"


def test_the_reconcile_helper_applies_wallet_truth_unconditionally() -> None:
    body = _function_body(_engine(), HELPER)
    calls = _calls(body, "risk::apply_wallet_truth")
    assert len(calls) == 1
    args = calls[0]
    assert args[0] == "*state_"
    assert args[2:4] == ["confirmed", "fields_validated"]
    # The bridge asset keeps its single writer while its scan is operational.
    assert "bridge_accounting_operational()" in body
    assert "get_position(" not in body


def test_state_is_booked_by_delta_only_in_the_bridge_scan() -> None:
    """The startup seed and Step 8 used record_buy, which adds: a position is
    then only as right as the balance it started from.  Both now set the
    balance outright.  The bridge scan keeps its own delta reconcile (its
    single-writer design, S19 rounds 4-6)."""
    text = _engine()
    everywhere = len(DELTA_WRITE.findall(text))
    in_bridge = len(DELTA_WRITE.findall(_function_body(text, BRIDGE_SCAN)))
    assert in_bridge >= 1
    assert everywhere == in_bridge, (
        f"engine.cpp books State by delta at {everywhere - in_bridge} site(s) "
        "outside step_ingest_bridge_flows"
    )


def test_startup_seeds_state_for_every_asset_after_the_read_loop() -> None:
    """The State seed runs after the try that holds every startup read, so a
    failure that ends that try early still seeds each asset; an asset it could
    not read is marked unverified.  Step 8 is the only place that clears the
    mark."""
    text = _engine()
    poll = _function_body(text, POLL_LOOP)

    # The seed's asset set is declared ahead of the try, so the code after the
    # try's catch can still loop over it.
    decl = re.search(r"std::unordered_set<std::string>\s+seed_asset_ids\s*;", poll)
    assert decl, "seed_asset_ids is not declared ahead of the seed's try"
    try_match = re.compile(r"\btry\s*\{").search(poll, decl.end())
    assert try_match, "no try follows the seed_asset_ids declaration"
    try_open = try_match.end() - 1
    try_close = _matching(poll, try_open)
    reads = poll[try_open:try_close]
    assert "offer_mgr_->ensure_wallet_ids()" in reads
    assert "wallet_->get_wallet_balance(" in reads
    after_try = poll[try_close + 1:]
    assert after_try.lstrip().startswith("catch"), "the reads' try has no catch"
    catch_open = poll.index("{", try_close + 1)
    catch_close = _matching(poll, catch_open)
    tail = poll[catch_close + 1:]

    seed_loop = re.search(r"for\s*\(\s*const\s+auto&\s*(\w+)\s*:\s*seed_asset_ids\s*\)", tail)
    assert seed_loop, "no loop over seed_asset_ids after the reads' catch"
    decisions = _calls(tail, "risk::decide_state_seed")
    assert len(decisions) == 1
    assert tail.index("risk::decide_state_seed(") > seed_loop.start()
    # The fallback quantity is the tracker's DB-restored one, taken BEFORE the
    # reads (review round 2): the read loop's seed_position() fills an empty
    # record from this boot's reply, which would then pass for persisted.
    assert decisions[0][2] == "persisted_quantity.at(%s)" % seed_loop.group(1), decisions[0][2]
    capture = re.search(r"persisted_quantity\s*\[\s*(\w+)\s*\]\s*=\s*"
                        r"inventory_->net_inventory\(\s*AssetId\{\s*\1\s*\}\s*\)\s*;", poll)
    assert capture, "the persisted quantities are not captured from the tracker"
    assert decl.end() < capture.start() < try_open, (
        "the persisted quantities must be captured before the reads' try"
    )
    assert "state_->reconcile_balance(" in tail

    lastknown_at = tail.index("risk::SeedSource::LastKnown")
    assert tail.index("state_unverified_assets_.insert(") > lastknown_at
    assert text.count("state_unverified_assets_.insert(") == 1


def _verification_block(body: str) -> tuple[int, str]:
    """Step 8's verification pass: the block guarded by the unverified set."""
    guard = re.search(r"if\s*\(\s*!\s*state_unverified_assets_\.empty\(\)\s*\)\s*\{", body)
    assert guard, "Step 8 has no verification pass"
    block_open = guard.end() - 1
    return guard.start(), body[block_open:_matching(body, block_open)]


def test_unverified_positions_are_verified_before_anything_is_posted() -> None:
    """Review round 1: Step 6 sized this heartbeat from the guess, so the pass
    runs below the sync gate but ahead of every offer action, clears a mark
    only when the wallet's balance actually replaced the guess, and a
    heartbeat that verified anything then stops -- the next one is sized from
    the wallet."""
    body = _function_body(_engine(), STEP8)
    start, block = _verification_block(body)
    assert body.index("wallet_->get_sync_status()") < start
    for first_action in ("retire_expired_offers(", "step_enforce_pace_caps(",
                         "post_quotes("):
        if first_action in body:
            assert start < body.index(first_action), first_action

    assert "reconcile_state_position(" in block

    def guarded(condition: str) -> str:
        """The body of the `if (<condition>) {` block inside the pass."""
        match = re.search(r"if\s*\(\s*" + condition + r"\s*\)\s*\{", block)
        assert match, f"no `if ({condition})` in the verification pass"
        open_at = match.end() - 1
        return block[open_at:_matching(block, open_at)]

    cleared = guarded(r"previous\.has_value\(\)")
    assert "state_unverified_assets_.erase(asset)" in cleared, (
        "a mark must be cleared only when a reconcile applied")
    assert "++verified" in cleared
    assert block.count("state_unverified_assets_.erase(") == 1
    # [review round 7] ...and records it for the drain, which it then reaches:
    # the heartbeat ends after the drain (test_a_verifying_heartbeat_drains_
    # before_it_ends), not here.
    assert "state_verified_undrained_.emplace(asset, block_height);" in cleared
    stops = guarded(r"verified\s*>\s*0")
    assert "verified_this_heartbeat = true;" in stops
    assert "co_return" not in stops, "the verifying heartbeat must still reach the drain"
    # A built wallet map with no wallet for the asset is a verified zero.
    assert "wallet_ids_resolved()" in block


def test_a_pair_with_an_unverified_position_is_not_quoted() -> None:
    """Review round 1: a position the pass could not read stays unverified,
    and the pair loop does not quote a pair that trades it -- ahead of its
    balance gate and of posting."""
    body = _function_body(_engine(), STEP8)
    loop_at = body.index('wallet_step_may_run("Step 8 pair loop")')
    gate = re.search(
        r"state_unverified_assets_\.count\(\s*\w+->base_asset_id\s*\)\s*>\s*0"
        r"\s*\|\|\s*state_unverified_assets_\.count\(\s*\w+->quote_asset_id\s*\)\s*>\s*0",
        body[loop_at:])
    assert gate, "the pair loop does not check its pair's positions are verified"
    gate_at = loop_at + gate.end()
    after = body[gate_at:]
    block_open = after.index("{")
    assert after[block_open:_matching(after, block_open)].rstrip().endswith("continue;")
    assert gate_at < body.index("get_wallet_balance(sb.wid)")
    assert gate_at < body.index("post_quotes(", loop_at)


def test_pace_managed_pairs_are_read_below_the_sync_gate_like_any_other() -> None:
    """Review round 4 (replacing rounds 1-2's pace branch): a pace-managed pair
    with an empty ladder took its State from pace's own read.  That read runs
    before Step 8's sync check, so a read taken while the wallet was still
    syncing reached State.  It covers only the assets pace lists, so XCH in a
    CAT-only pace config got no read at all.  The liveness refresh now reads
    a pace-managed pair's two assets itself, below the sync gate, like any
    other pair's."""
    body = _function_body(_engine(), STEP8)
    refresh = re.search(r"std::set<std::string>\s+refreshed\s*;", body)
    assert refresh, "the liveness refresh moved"
    block_open = body.rindex("{", 0, refresh.start())
    block = body[block_open:_matching(body, block_open)]
    assert "pace.managed" not in block, "the refresh must not set pace-managed pairs aside"
    assert "as_of_block" not in block, (
        "no State write from a cached read: every read here is made below the sync gate"
    )
    assert body.index("wallet_->get_sync_status()") < refresh.start()


def test_startup_reads_count_only_from_a_wallet_seen_synced() -> None:
    """Review round 4: the startup sync wait gives up when its probes run out,
    and boot carries on.  A balance read then -- or a "no wallet for this
    asset" answer from a wallet still finding its CAT wallets -- is no
    verified position.  The seed counts either only when the wait saw the
    wallet fully synced; otherwise every asset takes the unverified path, and
    Step 8 verifies it once the wallet is synced."""
    poll = _function_body(_engine(), POLL_LOOP)
    decl = re.search(r"bool\s+startup_wallet_synced\s*=\s*false\s*;", poll)
    assert decl, "the flag must start false"
    writes = re.findall(r"startup_wallet_synced\s*=\s*(\w+)\s*;", poll)
    assert writes == ["false", "true"], writes
    synced_branch = re.search(r"if\s*\(\s*synced\s*&&\s*!\s*syncing\s*\)\s*\{", poll)
    assert synced_branch and decl.start() < synced_branch.start()
    branch_open = synced_branch.end() - 1
    assert re.search(r"startup_wallet_synced\s*=\s*true\s*;",
                     poll[branch_open:_matching(poll, branch_open)]), (
        "only the probe that saw the wallet fully synced may set the flag"
    )
    read = re.search(r"if\s*\(\s*startup_wallet_synced\s*&&\s*"
                     r"bal_json\.contains\(\"confirmed_wallet_balance\"\)\s*\)\s*\{\s*"
                     r"state_seed_confirmed\[\s*aid\s*\]\s*=\s*confirmed\s*;", poll)
    assert read, "a startup balance counts as the wallet's word only from a synced wallet"
    miss = re.search(r"if\s*\(\s*startup_wallet_synced\s*&&\s*offer_mgr_->wallet_ids_resolved\(\)\s*\)"
                     r"\s*\{\s*state_seed_not_held\.insert\(\s*aid\s*\)\s*;", poll)
    assert miss, "a map miss counts as holding none only from a synced wallet"
    assert poll.count("state_seed_confirmed[") == 1
    assert poll.count("state_seed_not_held.insert(") == 1


def test_a_map_built_from_an_unsynced_wallet_is_dropped() -> None:
    """Review round 5: the startup seed builds the wallet-ID map, and
    ensure_wallet_ids() builds it only once.  Built from a wallet the sync
    wait never saw synced, it can lack a CAT wallet not yet created, and Step
    8's pass would read that CAT's -1 as a verified zero.  So the map is
    dropped after the seed when the wait did not see the wallet synced; Step
    8's pass then builds it below its sync gate."""
    text = _engine()
    poll = _function_body(text, POLL_LOOP)
    dropped = re.search(r"if\s*\(\s*!\s*startup_wallet_synced\s*&&\s*offer_mgr_\s*&&\s*"
                        r"offer_mgr_->wallet_ids_resolved\(\)\s*\)\s*\{\s*"
                        r"offer_mgr_->invalidate_wallet_ids\(\)\s*;\s*\}", poll)
    assert dropped, "a map built from an unsynced wallet must be dropped"
    assert poll.rindex("offer_mgr_->ensure_wallet_ids()", 0, dropped.start()) > poll.index(
        "bool startup_wallet_synced"), "the drop follows the seed's own map build"
    assert poll.index("risk::decide_state_seed(") < dropped.start(), (
        "the drop comes after every use the seed makes of the map"
    )
    _, block = _verification_block(_function_body(text, STEP8))
    assert "offer_mgr_->ensure_wallet_ids()" in block, (
        "Step 8's pass rebuilds the map, below its sync gate"
    )


def test_an_unverified_pair_takes_down_what_it_quotes() -> None:
    """Review round 2: offers restored at boot must not rest unmanaged for as
    long as a position read fails, so they are cancelled -- those not already
    cancelling -- and each is recorded as a submission.

    Review round 3: that drain sat in the pair loop, whose first line skips a
    pair with an empty ladder or an invalid quote -- which an unverified pair
    is likely to have.  It now scans the whole book before the pair loop:
    after this heartbeat's fees are set (every cancel in Step 8 pays them, and
    tests/test_fee_controller_wiring.py pins the fee setup ahead of the first
    selective_cancel), and ahead of every exit that follows."""
    text = _engine()
    body = _function_body(text, STEP8)
    drain = re.search(r"if\s*\(\s*\(\s*!\s*state_unverified_assets_\.empty\(\)\s*\|\|\s*"
                      r"!\s*state_verified_undrained_\.empty\(\)\s*\)\s*&&\s*offer_mgr_\s*&&\s*"
                      r"!\s*dry_run_\s*&&\s*!\s*cancel_all_inflight_\s*\)\s*\{", body)
    assert drain, "Step 8 has no drain"
    c1 = re.search(r'if\s*\(\s*!\s*wallet_step_may_run\("Step 8 \(offers\)"\)\s*\)\s*\{\s*co_return\s*;\s*\}',
                   body)
    assert c1, "Step 8's C1 circuit check moved"
    assert body.index("offer_mgr_->set_dynamic_fee(recommended_fee)") < c1.start() < drain.start()
    assert body.index("if (fee_tracker_->class_fees_active()) {") < drain.start()
    assert "co_return" not in body[c1.end():drain.start()], (
        "no exit may come between the fee setup's circuit check and the drain"
    )
    assert drain.start() < body.index("std::set<std::string> refreshed;")
    assert drain.start() < body.index('wallet_step_may_run("Step 8 pair loop")')
    drain_open = drain.end() - 1
    drained = body[drain_open:_matching(body, drain_open)]
    assert "state_->get_all_offers()" in drained, "the drain scans the whole book"
    pick = re.search(
        r"if\s*\(\s*drain_pc\s*&&\s*!\s*po\.cancel_pending\s*&&\s*\(\s*"
        r"drains\(\s*drain_pc->base_asset_id\s*,\s*po\s*\)\s*\|\|\s*"
        r"drains\(\s*drain_pc->quote_asset_id\s*,\s*po\s*\)\s*\)\s*\)",
        drained)
    assert pick, ("the drain must take every offer on a pair that trades an unverified "
                  "position, and only those not already cancelling")
    picked_open = drained.index("{", pick.end())
    assert "to_cancel.push_back(po.offer_id);" in drained[picked_open:_matching(drained, picked_open)]
    gate_at = drained.index('wallet_step_may_run("Step 8 unverified drain")')
    cancel_at = drained.index("offer_mgr_->selective_cancel(to_cancel)")
    assert pick.start() < gate_at < cancel_at
    marks = _calls(drained, "db_->mark_offer_cancel_submitted")
    assert marks == [["id", "block_height", '"unverified_position"']], marks
    assert text.count('"unverified_position"') == 1, (
        "one drain: the pair loop only skips an unverified pair"
    )


def test_the_liveness_refresh_reads_xch_too() -> None:
    """Review round 3: Step 7's XCH read updates the cap and never State, so for
    a pair that is not pace-managed, an empty ladder's refresh is the only read
    that brings XCH's State position to the wallet's.  XCH is refreshed like
    every other funding asset -- deduped, below the sync gate, reconciled."""
    body = _function_body(_engine(), STEP8)
    refresh = re.search(r"std::set<std::string>\s+refreshed\s*;", body)
    assert refresh, "the liveness refresh moved"
    assert body.index("wallet_->get_sync_status()") < refresh.start(), (
        "the refresh must stay below the sync gate"
    )
    loop = re.search(r"for\s*\(\s*const\s+auto&\s*asset\s*:\s*assets\s*\)\s*\{", body[refresh.end():])
    assert loop, "the liveness refresh no longer loops over a pair's two assets"
    loop_open = refresh.end() + loop.end() - 1
    loop_body = body[loop_open:_matching(body, loop_open)]
    assert not re.search(r'if\s*\(\s*asset\s*==\s*"xch"\s*\)\s*continue\s*;', loop_body), (
        "the liveness refresh must not skip XCH"
    )
    assert re.search(r"if\s*\(\s*!\s*refreshed\.insert\(\s*asset\s*\)\.second\s*\)\s*continue\s*;",
                     loop_body), "each asset is read once per heartbeat"
    assert _calls(loop_body, "reconcile_state_position") == [
        ["asset", "confirmed", "fields_validated", "block_height"]]


def test_the_drift_corrector_never_sizes_from_an_unverified_state() -> None:
    """Review round 2: Step 9f sizes TAKER trades and must not use a State
    position that is only a guess.  Review round 3: nor shares with an unread
    asset missing -- every other asset then looks overweight, and 9f, which
    trades both ways toward its targets, would sell them.  So it does nothing
    at all while any position is unverified: the gate comes first, before the
    cooldown and before any share is computed."""
    body = _function_body(_engine(), "asio::awaitable<void> Engine::step_run_drift_corrector(")
    guard = re.search(r"if\s*\(\s*!\s*state_unverified_assets_\.empty\(\)\s*\)\s*\{", body)
    assert guard, "9f is not gated on verification"
    guard_open = guard.end() - 1
    guard_close = _matching(body, guard_open)
    assert re.search(r"co_return\s*;", body[guard_open:guard_close]), "an unverified position must stop 9f"
    for later in ("last_drift_correction_block_", "cached_wallet_balances_",
                  "portfolio_pct_by_asset", "state_->get_all_positions()"):
        assert guard.start() < body.index(later), f"the gate must come before {later}"
    rearm = re.search(r"drift_unverified_warned_\s*=\s*false\s*;", body)
    assert rearm and rearm.start() > guard_close, (
        "the warning re-arms only once every position is verified"
    )
    assert body.count("state_unverified_assets_.empty()") == 1, "one gate, at the top"


def test_the_bridge_scan_verifies_only_from_a_wallet_step8_saw_synced() -> None:
    """Review round 6: the bridge scan runs every heartbeat, whether or not
    Step 8 got past its sync gate, and its own balance fetch checks only that
    the wallet answers.  So it clears its asset's unverified mark only when
    this heartbeat's Step 8 passed that gate: a flag cleared at the top of
    every heartbeat and set nowhere but just below the gate.  wallet_synced_
    would not do -- Step 8 does not run while paused, and it keeps its last
    value."""
    text = _engine()
    writes = re.findall(r"step8_sync_gate_passed_\s*=\s*(\w+)\s*;", text)
    assert writes == ["false", "true"], writes
    heartbeat = _function_body(text, HEARTBEAT)
    cleared = heartbeat.index("step8_sync_gate_passed_ = false;")
    assert cleared < heartbeat.index("co_await step_manage_offers(block_height)"), (
        "cleared before this heartbeat's Step 8 runs"
    )
    assert cleared < heartbeat.index("co_await step_ingest_bridge_flows(block_height)")
    body = _function_body(text, STEP8)
    set_at = body.index("step8_sync_gate_passed_ = true;")
    sync_at = body.index("wallet_->get_sync_status()")
    catch_at = body.index("catch (const std::exception& e) {", sync_at)
    catch_open = body.index("{", catch_at)
    assert _matching(body, catch_open) < set_at, "set only below the sync gate, after its catch"
    assert "co_return" not in body[_matching(body, catch_open) + 1:set_at]
    start, _ = _verification_block(body)
    assert set_at < start, "set before anything below the gate relies on it"


def test_the_bridge_scan_verifies_its_own_asset() -> None:
    """Review round 1: while its scan is operational the bridge asset has one
    State writer, so Step 8's pass leaves it alone and the scan clears its
    mark -- only once State holds the wallet's balance."""
    text = _engine()
    _, block = _verification_block(_function_body(text, STEP8))
    assert "bridge_accounting_operational()" in block
    bridge = _function_body(text, BRIDGE_SCAN)
    # Review round 6: and only from a wallet this heartbeat's Step 8 saw synced.
    assert re.search(r"step8_sync_gate_passed_\s*&&\s*"
                     r"state_->get_position\(\s*asset\s*\)\.balance\s*==\s*bal\.confirmed\s*"
                     r"&&\s*state_unverified_assets_\.erase\(\s*asset\s*\)", bridge)
    # Cleared in exactly those two places; the routine reconcile never does.
    assert text.count("state_unverified_assets_.erase(") == 2
    assert "state_unverified_assets_" not in _function_body(text, HELPER)


def test_a_verifying_heartbeat_drains_before_it_ends() -> None:
    """Review round 7: a heartbeat that verified a position returned before
    the drain, and on the next one the asset was no longer unverified, so the
    offers restored at boot on its pairs were never taken down.  Now both
    places that verify -- Step 8's pass and the bridge scan -- record the
    asset with the block it was verified at.  The drain takes, on its pairs,
    every offer created before that block (never one the pair loop posts
    afterwards), and forgets the assets once one pass has taken them all.  The
    verifying heartbeat ends right after the drain, before any posting, as it
    used to end at the pass.

    Review round 8: at or before that block.  After a restart within one peak
    a restored offer can carry the very height the first heartbeat verifies
    at, and a strict comparison left it resting while the drain forgot the
    asset."""
    text = _engine()
    body = _function_body(text, STEP8)
    drains = re.search(
        r"const auto drains = \[this\]\(const std::string& asset, const PendingOffer& po\) \{\s*"
        r"if \(state_unverified_assets_\.count\(asset\) > 0\) \{\s*return true;\s*\}\s*"
        r"const auto verified = state_verified_undrained_\.find\(asset\);\s*"
        r"return verified != state_verified_undrained_\.end\(\)\s*"
        r"&& po\.created_at_block <= verified->second;\s*\};", body)
    assert drains, "an unverified asset takes every offer; a verified one, what came at or before"
    drain_at = body.index("(!state_unverified_assets_.empty() || !state_verified_undrained_.empty())")
    assert drains.start() < drain_at
    drain_open = body.index("{", drain_at)
    drain = body[drain_open:_matching(body, drain_open)]
    assert re.search(r"if \(to_cancel\.empty\(\)\) \{\s*state_verified_undrained_\.clear\(\);\s*\}", drain), (
        "nothing older rests on their pairs: forget them")
    assert re.search(r"if \(done\.size\(\) == to_cancel\.size\(\)\) \{\s*"
                     r"state_verified_undrained_\.clear\(\);\s*\}", drain), (
        "every one went: forget them, and not before")
    assert text.count("state_verified_undrained_.clear()") == 2
    # The heartbeat that verified ends after the drain, before any posting.
    declared = body.index("bool verified_this_heartbeat = false;")
    ends = re.search(r"if \(verified_this_heartbeat\) \{\s*co_return;\s*\}", body)
    assert ends and declared < drain_at < ends.start(), "it must end after the drain"
    assert ends.start() > drain_open + len(drain)
    for posting in ("std::set<std::string> refreshed;", 'wallet_step_may_run("Step 8 pair loop")'):
        assert ends.start() < body.index(posting), posting
    assert body.count("verified_this_heartbeat") == 3, "declared, set by the pass, read once"
    # Both verifiers record the block.
    assert text.count("state_verified_undrained_.emplace(asset, block_height);") == 2
    bridge = _function_body(text, BRIDGE_SCAN)
    assert re.search(r"state_unverified_assets_\.erase\(asset\) > 0\) \{\s*"
                     r"state_verified_undrained_\.emplace\(asset, block_height\);", bridge), (
        "the bridge scan's verification must be drained too")
