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
    assert re.search(r"co_return\s*;", guarded(r"verified\s*>\s*0")), (
        "a heartbeat that verified a position must not go on to post")
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


def test_pace_managed_pairs_reconcile_from_a_fresh_pace_read() -> None:
    """Review round 1: the liveness refresh skips a pace-managed empty ladder,
    and nothing else reads its assets while it stays empty, so their State
    comes from the pace read made this heartbeat -- and only that heartbeat."""
    body = _function_body(_engine(), STEP8)
    managed = re.search(
        r"if\s*\(\s*config_\.strategy\.pace_enabled\s*&&\s*pcs\.pace\.managed\s*\)\s*\{", body)
    assert managed, "the liveness refresh no longer handles pace-managed pairs"
    block_open = managed.end() - 1
    block = body[block_open:_matching(body, block_open)]
    assert "reconcile_state_position(" in block
    assert re.search(r"as_of_block\s*==\s*block_height", block)
    assert block.rstrip().endswith("continue;")
    # Review round 2: XCH too -- refresh_pace_balances caches it as "xch", and
    # when every XCH pair is pace-managed with an empty ladder no other read
    # reaches its State position.
    assert '"xch"' not in block, "the pace reconciliation must not skip XCH"


def test_an_unverified_pair_takes_down_what_it_quotes() -> None:
    """Review round 2: the unverified gate's `continue` skips every cancel path
    in the pair loop, so offers restored at boot would rest unmanaged for as
    long as the read fails.  The gate cancels the pair's resting offers --
    those not already cancelling -- and records each as a submission."""
    body = _function_body(_engine(), STEP8)
    loop_at = body.index('wallet_step_may_run("Step 8 pair loop")')
    gate = re.search(
        r"state_unverified_assets_\.count\(\s*\w+->base_asset_id\s*\)\s*>\s*0"
        r"\s*\|\|\s*state_unverified_assets_\.count\(\s*\w+->quote_asset_id\s*\)\s*>\s*0",
        body[loop_at:])
    assert gate
    after = body[loop_at + gate.end():]
    block_open = after.index("{")
    block = after[block_open:_matching(after, block_open)]
    pick = re.search(r"if\s*\(\s*po\.pair_name\s*==\s*pair_name\s*&&\s*!\s*po\.cancel_pending\s*\)",
                     block)
    assert pick, "the drain must take exactly this pair's offers that are not already cancelling"
    picked_open = block.index("{", pick.end())
    assert "to_cancel.push_back(po.offer_id);" in block[picked_open:_matching(block, picked_open)]
    cancel_at = block.index("offer_mgr_->selective_cancel(to_cancel)")
    marks = _calls(block, "db_->mark_offer_cancel_submitted")
    assert marks == [["id", "block_height", '"unverified_position"']], marks
    assert pick.start() < cancel_at < block.index("mark_offer_cancel_submitted(")
    assert block.index("mark_offer_cancel_submitted(") < block.rindex("continue;")
    assert block.rstrip().endswith("continue;")


def test_the_drift_corrector_never_sizes_from_an_unverified_state() -> None:
    """Review round 2: Step 9f runs before Step 8's verification pass and sizes
    TAKER trades.  With no balance read yet it falls back on State, and it
    must not while any State position is unverified."""
    body = _function_body(_engine(), "asio::awaitable<void> Engine::step_run_drift_corrector(")
    fallback = re.search(r"if\s*\(\s*total_xch\s*<=\s*0\.0\s*\)\s*\{", body)
    assert fallback, "the drift corrector's State fallback moved"
    block_open = fallback.end() - 1
    block = body[block_open:_matching(body, block_open)]
    guard = re.search(r"if\s*\(\s*!\s*state_unverified_assets_\.empty\(\)\s*\)\s*\{", block)
    assert guard, "the State fallback is not gated on verification"
    guarded = block[guard.end() - 1:_matching(block, guard.end() - 1)]
    assert re.search(r"co_return\s*;", guarded), "an unverified State must stop 9f"
    assert guard.start() < block.index("state_->get_all_positions()"), (
        "the gate must come before State is read"
    )


def test_the_bridge_scan_verifies_its_own_asset() -> None:
    """Review round 1: while its scan is operational the bridge asset has one
    State writer, so Step 8's pass leaves it alone and the scan clears its
    mark -- only once State holds the wallet's balance."""
    text = _engine()
    _, block = _verification_block(_function_body(text, STEP8))
    assert "bridge_accounting_operational()" in block
    bridge = _function_body(text, BRIDGE_SCAN)
    assert re.search(r"get_position\(\s*asset\s*\)\.balance\s*==\s*bal\.confirmed\s*"
                     r"&&\s*state_unverified_assets_\.erase\(\s*asset\s*\)", bridge)
    # Cleared in exactly those two places; the routine reconcile never does.
    assert text.count("state_unverified_assets_.erase(") == 2
    assert "state_unverified_assets_" not in _function_body(text, HELPER)
