"""[FILL-PROOF 2026-09-23] Fill-proof wiring in the C++ engine, as a source scan.

LINT-CLASS GUARD, disclosed as such.  These tests read
cpp/src/execution/offer_manager.cpp, cpp/src/engine.cpp and
cpp/src/rpc/chia_rpc.cpp as TEXT.  They pin call-site WIRING that no C++ unit
test reaches -- Engine and OfferManager are not constructible in xop_tests --
while the DECISION is pinned by gtest (cpp/tests/test_fill_proof.cpp, which
replays the wallet and node records of a real phantom fill and of a real
take).  A pass here says the engine still asks that decision before it books a
fill; it says nothing about runtime behaviour.

What they guard.  detect_fills booked every offer the wallet reported
CONFIRMED.  On 2026-09-22 the wallet reported three offers CONFIRMED that were
never taken -- one input of each had been spent by another of the bot's own
transactions, and every other maker coin is still unspent -- and all three
entered trade_log, the ledger, the inventory tracker and State as fills.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "cpp" / "src" / "engine.cpp"
OFFER_MANAGER = REPO / "cpp" / "src" / "execution" / "offer_manager.cpp"
CHIA_RPC = REPO / "cpp" / "src" / "rpc" / "chia_rpc.cpp"

DETECT_FILLS = "asio::awaitable<std::vector<Fill>> OfferManager::detect_fills("
HANDLE_UNPROVEN = "void OfferManager::handle_unproven_fill("
PROVE_ON_CHAIN = "OfferManager::prove_fill_on_chain(const json& trade_record"
RECHECK_TERMINAL = "OfferManager::recheck_terminal(const std::string& trade_id,"
STEP_PROCESS_FILLS = "asio::awaitable<void> Engine::step_process_fills(BlockHeight block_height)"
WALLET_COIN_RECORDS = "ChiaWalletRPC::get_coin_records_by_names(const std::vector<std::string>& names)"


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


def _source(path: Path) -> str:
    return _strip_line_comments(_read(path))


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


def _block_after(text: str, head: str) -> str:
    """The `{...}` block that follows the first occurrence of `head`."""
    start = text.index(head)
    open_index = text.index("{", start + len(head) - 1)
    return text[open_index:_matching(text, open_index) + 1]


def _call_args(text: str, callee: str) -> list[list[str]]:
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
                args.append(" ".join(current.split()))
                current = ""
            else:
                current += ch
        args.append(" ".join(current.split()))
        results.append(args)
    return results


def _confirmed_branch() -> str:
    """detect_fills' CONFIRMED branch: everything it does for an offer the
    wallet reports CONFIRMED."""
    detect = _function_body(_source(OFFER_MANAGER), DETECT_FILLS)
    return _block_after(detect, "if (status == trade_status::kConfirmed) {")


def test_no_fill_is_booked_before_the_chain_proves_it():
    confirmed = _confirmed_branch()
    proof_at = confirmed.index("co_await prove_fill_on_chain(rec, ")
    gate = re.search(r"if \(proof\.verdict != FillProof::Settled\) \{\s*"
                     r"handle_unproven_fill\([^;]*\);\s*continue;\s*\}", confirmed)
    assert gate and gate.start() > proof_at, (
        "a proof that is not Settled must hand the offer to handle_unproven_fill "
        "and skip everything else"
    )
    for booking in ("Fill fill;", "state_->record_buy(", "state_->record_sell(",
                    "state_->remove_offer(", "fills.push_back("):
        assert confirmed.index(booking) > gate.end(), (
            "%s must come after the Settled gate" % booking
        )
    detect = _function_body(_source(OFFER_MANAGER), DETECT_FILLS)
    assert detect.count("fills.push_back(") == 1, (
        "the CONFIRMED branch must stay detect_fills' one booking point"
    )


def test_a_fill_counts_its_depth_from_the_proven_height():
    assignments = re.findall(r"fill\.block_height\s*=\s*([^;]+);", _confirmed_branch())
    assert assignments == ["static_cast<BlockHeight>(proof.height)"], (
        "the confirmation-depth buffer must count from the height the chain "
        "proved the take at, not the wallet's confirmed_at_index: %r" % assignments
    )


def test_an_unproven_fill_books_nothing_and_closes_only_a_deep_dead_offer():
    handler = _function_body(_source(OFFER_MANAGER), HANDLE_UNPROVEN)
    for booking in ("record_buy", "record_sell", "Fill ", "fills"):
        assert booking not in handler, "handle_unproven_fill must never book: %s" % booking

    head = "if (dead_offer_closable("
    closable = _block_after(handler, head)
    condition = handler[handler.index(head):handler.index(closable)]
    assert "strategy_cfg_.confirmation_depth_blocks" in condition, (
        "a Dead verdict is acted on only at the depth a fill waits for"
    )
    for step in ("state_->remove_offer(trade_id)", "proven_dead_.insert(trade_id)",
                 "last_dead_offers_.push_back(", "return;"):
        assert step in closable, "a closable dead offer must reach %s" % step
    rest = handler.replace(closable, "")
    for step in ("remove_offer(", "proven_dead_", "last_dead_offers_"):
        assert step not in rest, (
            "%s outside the closable branch would drop an offer that may yet be "
            "taken or proven" % step
        )


def test_recheck_terminal_never_readopts_a_proven_dead_offer():
    recheck = _function_body(_source(OFFER_MANAGER), RECHECK_TERMINAL)
    confirmed = _block_after(recheck, "if (status == trade_status::kConfirmed) {")
    guard = re.search(r"if \(proven_dead_\.count\(trade_id\) > 0\) \{\s*"
                      r"co_return TerminalRecheck::StillTerminal;\s*\}", confirmed)
    assert guard, "the CONFIRMED branch must answer StillTerminal for a proven-dead offer"
    assert guard.end() < confirmed.index('readopt("CONFIRMED")'), (
        "the guard must come before the re-adoption, or the wallet's CONFIRMED "
        "sends a dead offer round detect_fills again"
    )


def test_the_proof_asks_the_node_only_while_the_engine_trusts_it():
    wiring = _call_args(_source(ENGINE), "set_fill_proof_node")
    assert len(wiring) == 1, "the engine must wire the fill proof exactly once"
    node, trusted = wiring[0]
    assert node == "full_node_"
    assert "full_node_ != nullptr" in trusted and "!wallet_only_mode_" in trusted, (
        "the node is trusted by the S14 escalation's rule: present, and not "
        "wallet_only_mode_ -- read at call time: %r" % trusted
    )

    prove = _function_body(_source(OFFER_MANAGER), PROVE_ON_CHAIN)
    ask = prove.index("fill_proof_node_trusted_()")
    assert ask < prove.index("fill_proof_node_->get_coin_records_by_names("), (
        "the node is asked only after the trust predicate"
    )
    assert "wallet_->get_coin_records_by_names(names)" in prove, (
        "an untrusted node falls back to the wallet"
    )
    handler = _block_after(prove, "catch (const std::exception& e) {")
    assert "co_return FillProofResult{};" in handler, (
        "a failed lookup is Unknown -- no proof -- never a throw out of detect_fills"
    )
    assert "prove_fill(names, records, name_of)" in prove, (
        "the verdict is execution::prove_fill's, over the names that were asked for"
    )


def test_one_failed_lookup_ends_the_lookups_for_the_call():
    """A dead node or an unsynced wallet costs one failed lookup per heartbeat,
    not one per CONFIRMED offer -- each failure spends its transport retries,
    and the S14 escalation stops its sweep on the same failure."""
    detect = _function_body(_source(OFFER_MANAGER), DETECT_FILLS)
    latch = detect.index("bool proof_lookup_failed = false;")
    assert latch < detect.rindex("for (const auto& rec : trade_records) {"), (
        "the latch belongs to the call, declared before the loop that books"
    )
    asked = re.search(r"if \(proof_lookup_failed\) \{[^}]*\} else \{\s*"
                      r"proof = co_await prove_fill_on_chain\(rec, proof_failure, "
                      r"proof_lookup_failed\);\s*\}", _confirmed_branch())
    assert asked, "no lookup may be sent once one has failed in this call"
    prove = _function_body(_source(OFFER_MANAGER), PROVE_ON_CHAIN)
    handler = _block_after(prove, "catch (const std::exception& e) {")
    assert "lookup_failed = true;" in handler, "a failed lookup must set the latch"


def test_the_wallet_fallback_refuses_while_unsynced():
    body = _function_body(_source(CHIA_RPC), WALLET_COIN_RECORDS)
    assert '"include_spent_coins", true' in body, (
        "a take spends the maker coins: without spent coins nothing could be Settled"
    )
    assert "allow_unsynced" not in body, (
        "an unsynced wallet must refuse -- its coin store is incomplete exactly "
        "when it mislabelled dead offers CONFIRMED"
    )
    assert '"get_coin_records_by_names"' in body


def test_the_engine_records_every_dead_offer_as_cancelled_dead_on_chain():
    step = _function_body(_source(ENGINE), STEP_PROCESS_FILLS)
    detect_at = step.index("co_await offer_mgr_->detect_fills(")
    head = "for (const auto& dead : offer_mgr_->last_dead_offers()) {"
    assert step.index(head) > detect_at, "the dead list describes the detect_fills call above it"
    loop = _block_after(step, head)
    writes = _call_args(loop, "db_->update_offer_status")
    assert writes == [["dead.offer_id", '"cancelled"',
                       "static_cast<BlockHeight>(dead.spent_height)", '"dead_on_chain"']], (
        "a dead offer ends 'cancelled', resolved at the spend that killed it: %r" % writes
    )
    assert "buffer_terminal_offer" not in loop, (
        "not buffered: recheck_terminal would only hear the wallet say CONFIRMED again"
    )
    verdicts = _call_args(loop, "fee_feedback_on_cancel_verdict")
    assert len(verdicts) == 1 and verdicts[0][2].endswith("false"), (
        "a cancel we paid for on a dead offer did not confirm: its fee ticket "
        "closes unheard"
    )


def test_detect_fills_describes_only_its_own_call():
    detect = _function_body(_source(OFFER_MANAGER), DETECT_FILLS)
    assert detect.index("last_dead_offers_.clear();") < detect.index("co_return"), (
        "the dead list must be cleared before detect_fills can return, or the "
        "engine records the same offers again"
    )
