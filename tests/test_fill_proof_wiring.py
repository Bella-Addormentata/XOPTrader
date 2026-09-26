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
OFFER_MANAGER_HPP = REPO / "cpp" / "include" / "xop" / "execution" / "offer_manager.hpp"
FILL_PROOF_HPP = REPO / "cpp" / "include" / "xop" / "execution" / "fill_proof.hpp"
CHIA_RPC = REPO / "cpp" / "src" / "rpc" / "chia_rpc.cpp"

DETECT_FILLS = "asio::awaitable<std::vector<Fill>> OfferManager::detect_fills("
HANDLE_UNPROVEN = "void OfferManager::handle_unproven_fill("
PROVE_ON_CHAIN = "OfferManager::prove_fill_on_chain(const json& trade_record"
RECHECK_TERMINAL = "OfferManager::recheck_terminal(const std::string& trade_id,"
STEP_PROCESS_FILLS = "asio::awaitable<void> Engine::step_process_fills(BlockHeight block_height)"
WALLET_COIN_RECORDS = "ChiaWalletRPC::get_coin_records_by_names(const std::vector<std::string>& names)"
POLL_LOOP = "asio::awaitable<void> Engine::poll_loop_coro()"
STARTUP_RECONCILE = "asio::awaitable<std::vector<std::string>> OfferManager::startup_reconcile("
RETIRE_EXPIRED = "OfferManager::retire_expired_offers(BlockHeight current_block)"
RECONCILE_OFFERS = "asio::awaitable<std::vector<std::string>> OfferManager::reconcile_offers("


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
    head = "if (proof.verdict != FillProof::Settled) {"
    gate_at = confirmed.index(head)
    held = _block_after(confirmed, head)
    assert gate_at > proof_at and "handle_unproven_fill(" in held, (
        "a proof that is not Settled must hand the offer to handle_unproven_fill"
    )
    assert re.search(r"continue;\s*\}$", held), "...and skip everything else"
    gate_end = confirmed.index("{", gate_at + len(head) - 1) + len(held)
    for booking in ("Fill fill;", "state_->record_buy(", "state_->record_sell(",
                    "state_->remove_offer(", "fills.push_back("):
        assert confirmed.index(booking) > gate_end, (
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
    """A node or wallet that cannot answer -- a transport failure, or a reply
    that cannot be read -- costs one failed lookup per heartbeat, not one per
    CONFIRMED offer: each failure spends its transport retries, and the S14
    escalation stops its sweep on the same failure.

    [review #171, round 15] Not a lookup it refuses.  The wallet refuses a
    request naming a coin it does not hold, and that answer costs one round
    trip and is that offer's alone.  Latching on it let one offer, polled
    first in unordered_map order every heartbeat, keep every later fill
    unproven.

    [round 16] Only a refusal that names a missing coin.  A wallet that is not
    synced or not connected refuses every lookup the same way, so that one
    still ends the call's lookups (coin_lookup_refusal_is_offer_local, pinned
    by gtest on chia 2.7.4's own messages)."""
    detect = _function_body(_source(OFFER_MANAGER), DETECT_FILLS)
    latch = detect.index("bool proof_lookup_failed = false;")
    assert latch < detect.rindex("for (const auto& rec : trade_records) {"), (
        "the latch belongs to the call, declared before the loop that books"
    )
    asked = re.search(r"\} else if \(proof_lookup_failed\) \{[^}]*\} else \{\s*"
                      r"ProofLookup lookup = ProofLookup::Answered;\s*"
                      r"proof = co_await prove_fill_on_chain\(rec, proof_failure, lookup,\s*"
                      r"asked_node\);\s*"
                      r"if \(lookup == ProofLookup::Failed\) \{\s*"
                      r"proof_lookup_failed = true;\s*\}\s*\}", _confirmed_branch())
    assert asked, "no lookup once one has failed this call, and only a failed one latches"
    assert detect.count("proof_lookup_failed = true;") == 2, (
        "set by the two lookups, the CONFIRMED branch's and the re-proof's, and nowhere else"
    )
    prove = _function_body(_source(OFFER_MANAGER), PROVE_ON_CHAIN)
    for stage, anchor in (("first", "fill_proof_node_->get_coin_records_by_names("),
                          ("second", "fill_proof_node_->get_coin_records_by_parent_ids(")):
        rest = prove[prove.index(anchor):]
        refused_head = "catch (const rpc::ChiaRPCApplicationError& e) {"
        failed_head = "catch (const std::exception& e) {"
        assert rest.index(refused_head) < rest.index(failed_head), (
            f"{stage} stage: a refusal is caught before the catch-all can take it"
        )
        refused = _block_after(rest, refused_head)
        assert re.search(r"lookup = coin_lookup_refusal_is_offer_local\(e\.what\(\)\)\s*"
                         r"\? ProofLookup::Refused\s*: ProofLookup::Failed;", refused), (
            f"{stage} stage: a refused lookup is that offer's alone only when it names a missing coin"
        )
        assert "co_return FillProofResult{};" in refused, f"{stage} stage: ...and proves nothing"
        failed = _block_after(rest, failed_head)
        assert "lookup = ProofLookup::Failed;" in failed, (
            f"{stage} stage: a lookup that fails any other way sets the latch"
        )


def test_spent_together_books_only_with_the_takes_own_mark():
    """[review #171] Every maker coin spent in one block is what a take does,
    and also what a cancel or a stray spend of a one-coin offer does.  The
    second stage decides, from the node's settlement coin or the wallet's
    payment -- and a failure there is a failed lookup like any other."""
    prove = _function_body(_source(OFFER_MANAGER), PROVE_ON_CHAIN)
    stage1 = prove.index("prove_fill(names, records, name_of)")
    passthrough = re.search(r"if \(coins\.verdict != FillProof::SpentTogether\) \{\s*"
                            r"co_return coins;\s*\}", prove)
    assert passthrough and stage1 < passthrough.start(), (
        "only a SpentTogether verdict goes on to the second stage"
    )
    rest = prove[passthrough.end():]
    # [round 5] The node looks for each offered asset's settlement coin -- its
    # puzzle as well as its amount -- and the wallet for a requested amount.
    assert re.search(r"ask_node \? offered_settlements\(trade_record, puzzle_of\)", rest), (
        "the node looks for each offered asset's settlement coin"
    )
    assert re.search(r'ask_node \? std::vector<std::uint64_t>\{\} : '
                     r'summary_amounts\(trade_record, "requested"\)', rest), (
        "the wallet looks for a requested amount"
    )
    puzzle_of = _block_after(rest, "const auto puzzle_of = [](const std::string& asset) -> std::string {")
    assert "CoinManager::settlement_puzzle_hash(asset).value_or(std::string{})" in puzzle_of, (
        "each asset's settlement puzzle is named by CoinManager, as the chain's own coins show"
    )
    node = re.search(r"fill_proof_node_->get_coin_records_by_parent_ids\(\s*names", rest).start()
    assert node < rest.index("prove_take_from_children(coins, names, children, settlements)")
    wallet = rest.index("wallet_->get_coin_records_at_height(coins.height, amounts)")
    assert wallet < rest.index("prove_take_from_payments(coins, names, payments, amounts)")
    assert rest.index("if (ask_node) {") < node < rest.index("} else {") < wallet
    failed = _block_after(rest, "catch (const std::exception& e) {")
    assert "lookup = ProofLookup::Failed;" in failed and "co_return FillProofResult{};" in failed, (
        "a second-stage lookup that fails is Unknown and trips the latch"
    )
    assert "co_return coins;" not in rest, "a SpentTogether verdict never leaves unsettled"


def test_the_second_stage_asks_the_right_questions():
    """The node lists the children of the maker coins, spent ones included;
    the wallet lists our coins confirmed in exactly the spend block for the
    requested amounts (FilterMode.include is 1)."""
    rpc = _source(CHIA_RPC)
    children = _function_body(rpc, "ChiaFullNodeRPC::get_coin_records_by_parent_ids(")
    assert '"get_coin_records_by_parent_ids"' in children
    assert re.search(r'\{"parent_ids",\s*id_arr\}', children)
    assert re.search(r'\{"include_spent_coins",\s*include_spent\}', children)
    payments = _function_body(rpc, "ChiaWalletRPC::get_coin_records_at_height(")
    assert '"get_coin_records"' in payments
    assert re.search(r'\{"confirmed_range",\s*\{\{"start",\s*height\},\s*\{"stop",\s*height\}\}\}',
                     payments), "exactly the spend block, both ends inclusive"
    assert re.search(r'\{"amount_filter",\s*\{\{"values",\s*amounts\},\s*\{"mode",\s*1\}\}\}',
                     payments), "the requested amounts, included"
    # [round 5] No limit: an answer cut at N rows, asked the same way every
    # heartbeat, never shows a payment past the cut.  In chia 2.7.4 an omitted
    # limit is uint32 max, which get_coin_records serves with no LIMIT clause.
    assert '"limit"' not in payments, "the wallet's answer must not be cut short"
    # [round 5] A reply without its coin_records list is a failed lookup, not
    # an empty answer: an empty list of children now proves the offer Dead.
    for body in (children, payments):
        refused = re.search(r"if \(listed == resp\.end\(\) \|\| !listed->is_array\(\)\) \{\s*"
                            r"throw ChiaRPCError\(", body)
        assert refused and refused.start() < body.index("records.push_back("), (
            "a malformed reply must throw before anything is read from it"
        )
    prove = _function_body(_source(OFFER_MANAGER), PROVE_ON_CHAIN)
    assert re.search(r"get_coin_records_by_parent_ids\(\s*names,\s*/\*include_spent=\*/true\)",
                     prove), "a settlement coin is spent in the take block: include spent children"


def test_a_confirmed_offer_under_proof_is_never_cancelled():
    """[review #171] Chia 2.7.4's secure cancel sets PENDING_CANCEL over any
    status, and an insecure one sets CANCELLED.  So cancelling an offer the
    wallet reports CONFIRMED erases the CONFIRMED the proof waits on, and a
    real take with it.  Every per-offer cancel reaches the wallet through
    cancel_offer_charged.  The bulk cancel_offers endpoint skips completed
    trades itself."""
    manager = _source(OFFER_MANAGER)
    assert manager.count("wallet_->cancel_offer(") == 1, (
        "cancel_offer_charged must stay the only per-offer way to the wallet's cancel"
    )
    charged = _function_body(manager, "OfferManager::cancel_offer_charged(")
    guard = re.search(r"if \(cancel_withheld_for_proof\(trade_id\)\) \{", charged)
    assert guard and guard.start() < charged.index("wallet_->cancel_offer("), (
        "an offer under fill proof is refused before anything reaches the wallet"
    )
    assert "throw rpc::ChiaRPCError(" in _block_after(charged, guard.group(0)), (
        "refused like a wallet refusal, which every caller already handles"
    )
    emergency = _function_body(manager, "asio::awaitable<bool> OfferManager::emergency_cancel(")
    early = re.search(r"if \(cancel_withheld_for_proof\(offer_id\)\) \{", emergency)
    assert early and early.start() < emergency.index("get_wallet_balance("), (
        "emergency_cancel gives up at once, not after a balance read and a fee ladder "
        "of refusals"
    )
    assert "co_return false;" in _block_after(emergency, early.group(0))

    # Review round 2: withheld unless the LATEST proof makes it cancellable --
    # a Live verdict from the node, made by the latest detect_fills call (round
    # 4: the call, not the block), at depth past the claim.
    helper = _function_body(manager, "bool OfferManager::cancel_withheld_for_proof(")
    missing = re.search(r"if \(held == fill_proof_deferrals_\.end\(\)\) \{\s*return false;", helper)
    assert missing, "an offer not under proof is never withheld"
    decision = _call_args(helper, "live_offer_cancellable")
    assert decision == [["latest.verdict", "latest.from_node", "latest.claimed_height",
                         "latest.proved_block", "latest.proof_call", "fill_poll_heartbeat_",
                         "strategy_cfg_.confirmation_depth_blocks"]], decision
    assert re.search(r"return\s*!\s*live_offer_cancellable\(", helper)


def test_the_bulk_sweep_never_reports_a_proof_held_offer_cancelled():
    """[review #171, round 3] The wallet's cancel_offers sweep skips completed
    trades, CONFIRMED included, so an offer the fill proof holds is NOT
    cancelled by it.  cancel_all used to report every tracked id cancelled on
    a successful sweep and mark it cancel_pending.  A held offer now goes
    through the guarded per-offer path, and its outcome is reported as it is.

    [round 16] So does an offer State already marks cancel_pending.  The
    wallet may call it CANCELLED -- a row boot restored, an offer
    reconcile_offers or detect_fills keeps tracked -- which the sweep skips as
    it skips CONFIRMED, and it was reported as a cancel this call submitted."""
    manager = _source(OFFER_MANAGER)
    cancel_all = _function_body(
        manager, "asio::awaitable<OfferManager::CancelOutcome> OfferManager::cancel_all(")
    bulk_ok = _block_after(cancel_all, "if (bulk_ok) {")
    split = re.search(r"if \(fill_proof_deferrals_\.count\(po\.offer_id\) > 0U \|\| po\.cancel_pending\) \{\s*"
                      r"reread\.push_back\(po\.offer_id\);\s*\} else \{\s*"
                      r"out\.cancelled\.push_back\(po\.offer_id\);\s*\}", bulk_ok)
    assert split, "a proof-held offer, or one already cancel_pending, must not be reported cancelled"
    # [round 11] Nor does the re-read after the sweep: an offer it finds with a
    # cancel in flight, or closed, is reported as that, never as a cancel this
    # call submitted (test_the_sweep_is_judged_by_what_the_wallet_reports_after_it).
    assert bulk_ok.count("out.cancelled.push_back(") == 1, (
        "no other path in the bulk branch may report an id cancelled"
    )
    per_offer = bulk_ok.index("co_await cancel_ids(to_cancel, deadline)")
    assert per_offer > split.end(), "held offers go through the guarded per-offer path"
    after = bulk_ok[per_offer:]
    for field in ("cancelled", "failed", "already_pending"):
        assert re.search(r"out\.%s\.insert\(\s*out\.%s\.end\(\),\s*per_offer\.%s\.begin\(\),"
                         r"\s*per_offer\.%s\.end\(\)\);" % ((field,) * 4), after), (
            "the per-offer outcome must be kept: " + field)


def test_the_sweep_is_judged_by_what_the_wallet_reports_after_it():
    """[review #171, round 10] A hold records the LAST POLL's status, but the
    sweep acts on the status each trade has when it runs.  In chia 2.7.4 it
    cancels every PENDING_ACCEPT, PENDING_CONFIRM and PENDING_CANCEL trade,
    leaves each PENDING_CANCEL, and marks PENDING_CANCEL every trade not yet
    CANCELLED that shares a cancellation coin with one -- a held CONFIRMED
    trade included.  Sending every held offer down the per-offer path could
    therefore cancel one a second time, or report outstanding an offer the
    sweep covered.  So each held offer's status is read again after the sweep,
    and only one the wallet still reports CONFIRMED goes to the guard.  One that
    is live and was not swept is released and cancelled; one whose status cannot
    be read is sent nothing and reported outstanding.

    [round 11] Only a cancel this call's RPCs accepted is reported `cancelled`,
    which the callers persist as submitted with their own cause.  One with a
    cancel in flight is `already_pending`; one CANCELLED or FAILED is `closed`.
    PENDING_CANCEL and CANCELLED are what a cancel writes over a real take, so
    they keep the hold for detect_fills to prove on-chain; FAILED releases it."""
    manager = _source(OFFER_MANAGER)
    cancel_all = _function_body(
        manager, "asio::awaitable<OfferManager::CancelOutcome> OfferManager::cancel_all(")
    bulk_ok = _block_after(cancel_all, "if (bulk_ok) {")
    held_branch = _block_after(bulk_ok, "if (!reread.empty()) {")
    loop_head = "for (std::size_t i = 0; i < reread.size(); ++i) {"
    assert held_branch.index(loop_head) < held_branch.index("co_await cancel_ids("), (
        "every held offer is re-read before any is cancelled"
    )
    reread = _block_after(held_branch, loop_head)
    assert reread[1:].lstrip().startswith("const auto& oid = reread[i];")

    # The deadline comes first, and whatever it cuts short is reported, not dropped.
    deadline = re.search(r"if \(std::chrono::steady_clock::now\(\) >= deadline\) \{\s*"
                         r"for \(std::size_t j = i; j < reread\.size\(\); \+\+j\) \{\s*"
                         r"unread\.push_back\(reread\[j\]\);\s*\}\s*"
                         r"out\.deadline_hit = true;\s*break;\s*\}", reread)
    assert deadline and deadline.start() < reread.index("co_await wallet_->get_offer("), (
        "the re-read stops at the deadline and reports the rest outstanding"
    )
    # One read per offer, and a read that fails leaves no status at all.
    read = re.search(r"int status = -1;\s*json rec;\s*try \{\s*"
                     r"rec = co_await wallet_->get_offer\(oid, /\*file_contents=\*/false\);\s*"
                     r"if \(const auto st = rec\.find\(\"status\"\); st != rec\.end\(\)\) \{\s*"
                     r"status = trade_status::parse\(\*st\);\s*\}\s*"
                     r"\} catch \(const std::exception& e\) \{\s*read_error = e\.what\(\);\s*\}",
                     reread)
    assert read, "the status is read after the sweep, and a failed read is no status"
    # The decision, in full.
    # [round 18] CANCELLED has a block of its own, proven first
    # (test_a_cancelled_offer_is_closed_by_the_sweep_only_once_the_chain_says_so).
    cancelled_head = "} else if (status == trade_status::kCancelled) {"
    cancelled_block = _block_after(reread, cancelled_head)
    reread = reread.replace(cancelled_head[:-1] + cancelled_block, cancelled_head + " CANCELLED }", 1)
    decision = re.search(
        r"if \(status == trade_status::kConfirmed\) \{\s*to_cancel\.push_back\(oid\);\s*"
        r"\} else if \(status == trade_status::kPendingCancel\) \{\s*"
        r"out\.already_pending\.push_back\(oid\);\s*\+\+in_flight;\s*"
        r"\} else if \(status == trade_status::kCancelled\) \{ CANCELLED \}\s*"
        r"else if \(status == trade_status::kFailed\) \{\s*"
        r"fill_proof_deferrals_\.erase\(oid\);\s*out\.closed\.push_back\(oid\);\s*\+\+closed;\s*"
        r"\} else if \(trade_status::is_known\(status\)\) \{\s*"
        r"fill_proof_deferrals_\.erase\(oid\);\s*to_cancel\.push_back\(oid\);\s*"
        r"\} else \{\s*unread\.push_back\(oid\);\s*\}", reread)
    assert decision and decision.start() > read.end(), (
        "CONFIRMED goes to the guard; a cancel in flight is already pending and a closed "
        "offer is closed, both still held unless FAILED; any other real status is released "
        "and cancelled; anything else is unread"
    )
    assert "out.cancelled.push_back(" not in held_branch, (
        "the re-read reports nothing as a cancel this call submitted"
    )
    assert held_branch.count("fill_proof_deferrals_.erase(") == 2, (
        "only FAILED and a live offer the sweep missed are released"
    )
    # Only the re-read's picks are cancelled one by one.  An unread offer is
    # reported outstanding: never cancelled, never reported cancelled.
    assert held_branch.count("to_cancel.push_back(") == 2
    assert held_branch.count("unread.push_back(") == 2
    assert "co_await cancel_ids(to_cancel, deadline)" in held_branch
    kept = re.search(r"if \(!unread\.empty\(\)\) \{\s*"
                     r"out\.failed\.insert\(out\.failed\.end\(\), unread\.begin\(\), unread\.end\(\)\);",
                     held_branch)
    assert kept and kept.start() > held_branch.index("co_await cancel_ids("), (
        "an offer whose status is unknown is reported outstanding"
    )


def test_a_cancelled_offer_is_closed_by_the_sweep_only_once_the_chain_says_so():
    """[review #171, round 18] After an accepted sweep, the re-read reported an
    offer the wallet calls CANCELLED as closed.  But a local cancel leaves every
    maker coin unspent and the offer takeable, the sweep skips it as it skips
    every completed trade, and every caller ignores `closed`: a shutdown could
    end "all cancelled" with it still on offer.  Now such an offer is proven
    on-chain first.  Dead or taken is closed.  Live, or not proven, is
    outstanding, in `failed`, with nothing sent.  And it stays outstanding on a
    retry: an offer proven CANCELLED and Live is remembered (local_cancel_live_),
    and cancel_ids reports it in `failed`, never as a cancel already in flight."""
    manager = _source(OFFER_MANAGER)
    cancel_all = _function_body(
        manager, "asio::awaitable<OfferManager::CancelOutcome> OfferManager::cancel_all(")
    bulk_ok = _block_after(cancel_all, "if (bulk_ok) {")
    reread_branch = _block_after(bulk_ok, "if (!reread.empty()) {")
    cancelled = _block_after(reread_branch, "} else if (status == trade_status::kCancelled) {")
    asked = re.search(r"if \(proof_lookup_failed\) \{[^}]*\} else \{\s*"
                      r"proof = co_await prove_fill_on_chain\(rec, proof_failure, lookup,\s*asked_node\);\s*"
                      r"if \(lookup == ProofLookup::Failed\) \{\s*proof_lookup_failed = true;\s*\}\s*\}",
                      cancelled)
    assert asked, "proven on-chain, and not asked again in this sweep once a lookup has failed"
    # [round 19] Dead only at confirmation depth, judged at the latest fill poll.
    closed = re.search(r"if \(proof\.verdict == FillProof::Settled\s*"
                       r"\|\| dead_offer_closable\(proof, latest_fill_poll_block_,\s*"
                       r"strategy_cfg_\.confirmation_depth_blocks\)\) \{\s*"
                       r"local_cancel_live_\.erase\(oid\);\s*"
                       r"out\.closed\.push_back\(oid\);\s*\+\+closed;\s*\} else \{", cancelled)
    assert closed and asked.end() < closed.start(), "only a taken offer, or one dead at depth, is closed"
    other = cancelled[closed.end() - 1:_matching(cancelled, closed.end() - 1) + 1]
    # [round 20] Whatever the answer, one it does not close is flagged and
    # remembered first, so every retry keeps it outstanding and nothing
    # cancels it again: Live, a shallow Dead, or not proven.
    assert re.match(r"\{\s*state_->mark_cancel_pending\(oid\);\s*local_cancel_live_\.insert\(oid\);\s*"
                    r"if \(proof\.verdict == FillProof::Live\) \{", other), (
        "flagged and remembered before its answer is read")
    assert other.count("local_cancel_live_.insert(oid);") == 1
    assert re.search(r'\} else if \(proof\.verdict == FillProof::Dead\) \{\s*takeable_why = "dead at block "',
                     other), "a shallow Dead one is told apart in the report"
    assert re.search(r"\+\+fill_poll_heartbeat_;\s*latest_fill_poll_block_ = current_block;",
                     _function_body(manager, DETECT_FILLS)), "the block the depth is judged at"
    assert manager.count("latest_fill_poll_block_ =") == 1
    assert re.search(r"takeable\.push_back\(oid\);\s*\}$", other), "Live or not proven: outstanding"
    assert "out.closed" not in other and "out.cancelled" not in other
    kept = re.search(r"if \(!takeable\.empty\(\)\) \{[^}]*"
                     r"out\.failed\.insert\(out\.failed\.end\(\), takeable\.begin\(\), takeable\.end\(\)\);",
                     reread_branch)
    assert kept and kept.start() > reread_branch.index("co_await cancel_ids("), (
        "reported outstanding, with nothing sent for it"
    )
    # And on every retry: cancel_ids reports it outstanding, never already pending.
    ids = _function_body(manager, "asio::awaitable<OfferManager::CancelOutcome> OfferManager::cancel_ids(")
    live = re.search(r"if \(po\.cancel_pending && local_cancel_live_\.count\(oid\) > 0U\) \{", ids)
    pending = ids.index("if (po.cancel_pending) {")
    assert live and live.start() < pending, "checked before the cancel-in-flight skip"
    block = ids[live.end() - 1:_matching(ids, live.end() - 1) + 1]
    assert re.search(r"out\.failed\.push_back\(oid\);\s*continue;\s*\}$", block), "outstanding, nothing sent"
    assert "already_pending" not in block and "cancel_offer_charged" not in block
    # [round 20] Verdict-neutral: a shallow Dead one is not "still takeable".
    assert re.search(r'const std::string why = "the wallet reports " \+ oid\.substr\(0, 12\)\s*'
                     r'\+ " CANCELLED, but the chain has not yet shown it taken, or dead "\s*'
                     r'"at confirmation depth";', block), "the report names what is not yet proven"
    # detect_fills remembers what it proves, and forgets it on a Dead or taken proof.
    detect = _function_body(manager, DETECT_FILLS)
    assert re.search(r"state_->mark_cancel_pending\(trade_id\);\s*local_cancel_live_\.insert\(trade_id\);", detect), (
        "the round-15 keep branch remembers a Live CANCELLED offer"
    )
    assert re.search(r"if \(dead_at_depth \|\| reproof\.verdict == FillProof::Settled\) \{\s*"
                     r"local_cancel_live_\.erase\(trade_id\);\s*\}", detect), (
        "[round 19] forgotten only on a take or a Dead proof at depth")
    assert re.search(r"for \(auto it = local_cancel_live_\.begin\(\); it != local_cancel_live_\.end\(\);\) \{\s*"
                     r"it = pending_map\.count\(\*it\) \? std::next\(it\) : local_cancel_live_\.erase\(it\);\s*\}",
                     detect), "pruned with the deferrals"
    assert "local_cancel_live_.clear();" in _block_after(detect, "if (pending_offers.empty()) {")
    assert re.search(r"std::unordered_set<std::string>\s+local_cancel_live_;", _source(OFFER_MANAGER_HPP))


def test_the_latest_proof_is_what_a_cancel_is_judged_by():
    """[review #171, round 2] The guard reads what the last proof found, from
    where, against which claim, and when -- so a CONFIRMED offer the node has
    proven live again can be cancelled -- and an offer leaves the guard as soon
    as the wallet stops calling it CONFIRMED."""
    manager = _source(OFFER_MANAGER)
    detect = _function_body(manager, DETECT_FILLS)
    # [round 4] A proof belongs to a detect_fills CALL, numbered by the poll
    # heartbeat counter.  It is advanced once per call, before the call's
    # first await, and written nowhere else, so while a call waits, no proof
    # an earlier call made can pass for this one's.
    assert detect.index("++fill_poll_heartbeat_;") < detect.index("co_await"), (
        "the call is numbered before its first await"
    )
    writes = re.findall(r"\+\+\s*fill_poll_heartbeat_|--\s*fill_poll_heartbeat_"
                        r"|fill_poll_heartbeat_\s*(?:\+\+|--|[-+*/]?=(?!=))", manager)
    assert writes == ["++fill_poll_heartbeat_"], writes
    # [round 11] ...other than one a cancel writes, which may hide a take.
    cleared = re.search(r"if \(status != trade_status::kConfirmed && trade_status::is_known\(status\)\s*"
                        r"&& !trade_status::written_by_a_cancel\(status\)\) \{\s*"
                        r"fill_proof_deferrals_\.erase\(trade_id\);\s*\}", detect)
    assert cleared and cleared.start() < detect.index("if (status == trade_status::kConfirmed) {"), (
        "an offer the wallet no longer calls CONFIRMED is not under proof"
    )
    confirmed = _confirmed_branch()
    assert re.search(r"claimed_height = idx->get<std::uint64_t>\(\);", confirmed), (
        "the wallet's claimed height comes from the record's confirmed_at_index"
    )
    unproven = _call_args(confirmed, "handle_unproven_fill")
    assert unproven == [["trade_id", "po", "proof", "proof_failure", "asked_node",
                         "claimed_height", "current_block"]], unproven
    prove = _function_body(manager, PROVE_ON_CHAIN)
    assert re.search(r"asked_node = ask_node;", prove), "the proof's source must be reported"
    handler = _function_body(manager, HANDLE_UNPROVEN)
    for field, value in (("verdict", "proof.verdict"), ("from_node", "from_node"),
                         ("claimed_height", "claimed_height"), ("proved_block", "current_block"),
                         ("proof_call", "fill_poll_heartbeat_")):
        assert re.search(r"deferral\.%s\s*=\s*%s;" % (field, re.escape(value)), handler), field


def test_an_offer_is_held_from_the_moment_the_wallet_says_confirmed():
    """[review #171, round 4] The guard was entered only after the offer's own
    proof lookup.  Every await before that -- this call's later polls included
    -- let a detached Cancel All or the shutdown ladder run on the engine's one
    io_context, find no guard, and report the offer cancelled or overwrite the
    CONFIRMED the proof waits on.  The poll that reads CONFIRMED now holds the
    offer, with no await in between, and a new entry is never cancellable.

    [review #171, round 6] The same poll releases the hold when it reads any
    other status.  Left for the erase after the loop, the stale hold refused,
    at every later await, the cancel of an offer the wallet had already taken
    out of CONFIRMED."""
    manager = _source(OFFER_MANAGER)
    detect = _function_body(manager, DETECT_FILLS)
    poll = detect.index("co_await wallet_->get_offer(trade_id,")
    # [round 8] Released only by a status the wallet really reported: an
    # unrecognised one is no evidence the offer left CONFIRMED.
    # [round 11] ...and not by a status a cancel writes, which may hide a take.
    hold = re.search(r"if \(const auto st = rec\.find\(\"status\"\); st != rec\.end\(\)\) \{\s*"
                     r"const int polled = trade_status::parse\(\*st\);\s*"
                     r"if \(polled == trade_status::kConfirmed\) \{\s*"
                     r"fill_proof_deferrals_\.try_emplace\(trade_id\);\s*"
                     r"\} else if \(trade_status::is_known\(polled\)\s*"
                     r"&& !trade_status::written_by_a_cancel\(polled\)\) \{\s*"
                     r"fill_proof_deferrals_\.erase\(trade_id\);\s*"
                     r"\}\s*\}", detect)
    known = re.search(r"constexpr bool is_known\(int status\) noexcept \{\s*"
                      r"return status >= kPendingAccept && status <= kFailed;\s*\}", manager)
    assert known, "is_known must admit exactly Chia's six TradeStatus codes"
    assert re.search(r"constexpr int kPendingAccept\s*= 0;", manager)
    assert re.search(r"constexpr int kFailed\s*= 5;", manager)
    assert hold and hold.start() > poll, (
        "the poll holds an offer it reads CONFIRMED and releases one it reads otherwise"
    )
    assert "co_await" not in detect[poll + len("co_await"):hold.start()], (
        "an await between reading the status and holding or releasing the offer reopens the window"
    )
    assert hold.end() < detect.index("trade_records.push_back(std::move(rec));")
    assert hold.end() < detect.index("co_await prove_fill_on_chain(")
    deferral = _block_after(_source(OFFER_MANAGER_HPP), "struct FillProofDeferral {")
    assert re.search(r"FillProof\s+verdict\{\};", deferral), "a new entry is Unknown"
    assert re.search(r"std::uint64_t\s+proof_call\{0\};", deferral), "a new entry is from no call"


def test_a_cancels_status_is_proven_on_chain_before_it_is_acted_on():
    """[review #171, round 11] A cancel's status can hide a real take.  chia
    2.7.4's cancel_pending_offers writes PENDING_CANCEL (CANCELLED if insecure)
    on the trade it cancels AND on every trade not yet CANCELLED that shares a
    cancellation coin with it, a CONFIRMED one included.  And a real take does
    not fail a pending offer that shared one of its coins, so cancelling that
    offer later -- by any path: this engine's sweeps and per-offer cancels, the
    watchdog's sweep -- overwrote the take's CONFIRMED, and the take was never
    proved or booked.

    [round 12] Nor only for an offer held here, which this process saw
    CONFIRMED: the overwrite can come before that poll, and a restart forgets
    every hold.  An offer is proven the first time it shows each cancel status
    (cancel_status_proven_ remembers the answer), and while held, every
    heartbeat.  Settled books it as the CONFIRMED offer it was, from that proof.
    Live or Dead lets the status stand and releases any hold.  Unknown keeps a
    held offer held; one not held is asked again only after a failed lookup,
    and otherwise its status stands.

    [round 14] A PENDING_CANCEL also marks State cancel_pending first, as
    recheck_terminal's revival does, so once a proof releases the hold no TTL
    or reprice path sends a second cancel.  And only Dead is remembered: a Live
    offer can still be taken, so it is asked again next heartbeat."""
    manager = _source(OFFER_MANAGER)
    written = re.search(r"constexpr bool written_by_a_cancel\(int status\) noexcept \{\s*"
                        r"return status == kPendingCancel \|\| status == kCancelled;\s*\}", manager)
    assert written, "exactly the two statuses a cancel writes"
    detect = _function_body(manager, DETECT_FILLS)
    head = "for (const auto& rec : trade_records) {"
    loop = _block_after(detect[detect.rindex(head):], head)
    gate = re.search(r"const bool held = fill_proof_deferrals_\.count\(trade_id\) > 0U;\s*"
                     r"bool reprove = false;\s*"
                     r"if \(trade_status::written_by_a_cancel\(status\)\) \{\s*"
                     r"const auto seen = cancel_status_proven_\.find\(trade_id\);\s*"
                     r"reprove = held \|\| seen == cancel_status_proven_\.end\(\) "
                     r"\|\| seen->second != status;\s*\}", loop)
    assert gate, "proven while held, and the first time each cancel status shows"
    fallback = loop.index("fill_proof_deferrals_.erase(trade_id);")
    opener = loop.index("if (reprove) {")
    branch = loop.index("if (status == trade_status::kConfirmed) {")
    assert fallback < gate.start() < opener < branch, (
        "proven after the fallback release, and before the CONFIRMED branch books"
    )
    declared = loop.index("std::optional<FillProofResult> reproved;")
    assert declared < opener, "one re-proof per record, never carried to the next"
    # [round 14] A PENDING_CANCEL marks State cancel_pending, before any proof
    # can release the hold, so no TTL or reprice path sends a second cancel.
    mark = re.search(r"if \(status == trade_status::kPendingCancel\) \{\s*"
                     r"state_->mark_cancel_pending\(trade_id\);\s*\}", loop)
    assert mark and fallback < mark.start() < gate.start(), (
        "a cancel in flight must be marked before the proof, on every path"
    )
    reproof = _block_after(loop, "if (reprove) {")
    asked = re.search(r"ProofLookup reproof_lookup = ProofLookup::Answered;\s*"
                      r"if \(proof_lookup_failed\) \{\s*reproof_lookup = ProofLookup::Failed;[^}]*"
                      r"\} else \{\s*"
                      r"reproof = co_await prove_fill_on_chain\(rec, reproof_failure,\s*"
                      r"reproof_lookup,\s*reproved_asked_node\);\s*"
                      r"if \(reproof_lookup == ProofLookup::Failed\) \{\s*"
                      r"proof_lookup_failed = true;\s*\}\s*\}", reproof)
    assert asked, "asked once, not at all once a lookup has failed this call, and only a failed one latches"
    settled = _block_after(reproof, "if (reproof.verdict == FillProof::Settled) {")
    assert re.search(r"reproved = reproof;\s*status = trade_status::kConfirmed;\s*\}$", settled), (
        "a take after all: booked by the CONFIRMED branch, from this proof"
    )
    # [round 14] Only Dead is remembered: Live can still be taken.
    # [round 16] Dead only at confirmation depth
    # (test_a_dead_answer_is_final_only_at_confirmation_depth).
    no_take = re.search(r"\} else if \(reproof\.verdict == FillProof::Live \|\| dead_at_depth\) \{\s*"
                        r"fill_proof_deferrals_\.erase\(trade_id\);\s*"
                        r"if \(dead_at_depth\) \{\s*"
                        r"cancel_status_proven_\[trade_id\] = status;\s*"
                        r"\} else if \(status == trade_status::kCancelled\s*"
                        r"&& expiry_retired_\.count\(trade_id\) == 0U\) \{", reproof)
    assert no_take, "no take: the status stands and any hold goes; only Dead is not asked again"
    held_unknown = _block_after(reproof, "} else if (held) {")
    assert _call_args(held_unknown, "handle_unproven_fill") == [[
        "trade_id", "po", "reproof", "reproof_failure", "reproved_asked_node",
        "claimed_height", "current_block",
        'status == trade_status::kPendingCancel ? "PENDING_CANCEL" : "CANCELLED"']], (
        "a held Unknown is deferred as a CONFIRMED offer's is, [#172's review] under the status it shows")
    assert re.search(r"continue;\s*\}$", held_unknown), "...still held, and asked again next heartbeat"
    assert "fill_proof_deferrals_.erase(" not in held_unknown, "a held Unknown must not let the offer go"
    # [round 15] Its own lookup, refused or failed, or an earlier one's latch.
    waits = re.search(r"\} else if \(reproof_lookup == ProofLookup::Refused\s*"
                      r"\|\| reproof_lookup == ProofLookup::Failed\) \{", reproof)
    assert waits, "a refused or failed lookup, or an earlier one's latch, waits"
    transient = reproof[waits.end() - 1:_matching(reproof, waits.end() - 1) + 1]
    assert re.search(r"continue;\s*\}$", transient), "after a failed lookup: asked again next heartbeat"
    assert "cancel_status_proven_" not in transient and "handle_unproven_fill" not in transient, (
        "a failed lookup is remembered as no answer, and holds nothing"
    )
    assert re.search(r"\} else \{\s*cancel_status_proven_\[trade_id\] = status;\s*\}\s*\}$", reproof), (
        "with nothing to ask, the chain can say no more, and the status stands"
    )
    # [#172's review] Dead at depth, an answer that settled nothing once its
    # window has passed, and nothing to ask.
    assert reproof.count("cancel_status_proven_[trade_id] = status;") == 3
    reuse = re.search(r"if \(reproved\) \{\s*proof\s*=\s*\*reproved;\s*"
                      r"asked_node\s*=\s*reproved_asked_node;\s*"
                      r"\} else if \(proof_lookup_failed\) \{", _confirmed_branch())
    assert reuse, "the CONFIRMED branch books a re-proved take from that proof, asking nothing twice"
    # What is remembered goes with the offer.
    pruned = re.search(r"for \(auto it = cancel_status_proven_\.begin\(\); "
                       r"it != cancel_status_proven_\.end\(\);\) \{\s*"
                       r"it = pending_map\.count\(it->first\) \? std::next\(it\) "
                       r": cancel_status_proven_\.erase\(it\);\s*\}", detect)
    assert pruned and pruned.start() < detect.rindex(head), "pruned before the loop, as the deferrals are"
    assert "cancel_status_proven_.clear();" in _block_after(detect, "if (pending_offers.empty()) {")
    assert re.search(r"std::unordered_map<std::string, int>\s+cancel_status_proven_;",
                     _source(OFFER_MANAGER_HPP))


def test_a_dead_answer_is_final_only_at_confirmation_depth():
    """[review #171, round 16] A Dead re-proof under a cancel's status was
    remembered at once, and a CANCELLED offer closed on it, whatever the depth
    of the spend that killed it.  The CONFIRMED path waits until that spend is
    confirmation_depth_blocks deep (dead_offer_closable), because a shallower
    one can be reorganised out -- and in chia 2.7.4 a reorganisation leaves
    the wallet's trade records alone, so the cancel's status would stay over
    an offer that can be taken again.  The re-proof now uses the same rule.
    Dead at depth is final.  A shallower Dead keeps a held offer held, through
    handle_unproven_fill as a CONFIRMED offer's would be, and keeps any other
    tracked and cancel_pending, remembering nothing, so it is proven again
    next heartbeat."""
    manager = _source(OFFER_MANAGER)
    detect = _function_body(manager, DETECT_FILLS)
    head = "for (const auto& rec : trade_records) {"
    loop = _block_after(detect[detect.rindex(head):], head)
    reproof = _block_after(loop, "if (reprove) {")
    depth = re.search(r"const bool dead_at_depth = dead_offer_closable\(\s*"
                      r"reproof, current_block, strategy_cfg_\.confirmation_depth_blocks\);", reproof)
    assert depth, "judged by the CONFIRMED path's own rule"
    assert reproof.index("co_await prove_fill_on_chain(") < depth.start() < reproof.index(
        "if (reproof.verdict == FillProof::Settled) {"), "once the re-proof is in, before any verdict acts"
    final = re.search(r"\} else if \(reproof\.verdict == FillProof::Live \|\| dead_at_depth\) \{", reproof)
    assert final, "only a Live answer, or a Dead one at depth, lets the status stand"
    assert re.search(r"if \(dead_at_depth\) \{\s*cancel_status_proven_\[trade_id\] = status;", reproof), (
        "and only a Dead one at depth is remembered"
    )
    # [round 19] Read directly once again: a live local cancel is forgotten
    # only at depth too (dead_at_depth), never on a shallow Dead.
    assert reproof.count("FillProof::Dead") == 1, (
        "a Dead verdict is read directly only where it is not yet deep enough"
    )
    assert re.search(r"if \(dead_at_depth \|\| reproof\.verdict == FillProof::Settled\) \{\s*"
                     r"local_cancel_live_\.erase\(trade_id\);\s*\}", reproof)
    held_at = reproof.index("} else if (held) {")
    shallow = re.search(r"\} else if \(reproof\.verdict == FillProof::Dead\) \{", reproof)
    assert shallow and final.start() < held_at < shallow.start() < reproof.index(
        "} else if (reproof_lookup == ProofLookup::Refused"), (
        "a held offer's shallow Dead is deferred as a CONFIRMED offer's is; any other's is "
        "handled next, before a failed lookup's wait"
    )
    open_index = shallow.end() - 1
    block = reproof[open_index:_matching(reproof, open_index) + 1]
    assert re.match(r"\{\s*state_->mark_cancel_pending\(trade_id\);", block), (
        "flagged cancel_pending, so nothing cancels it again meanwhile"
    )
    assert re.search(r"continue;\s*\}$", block), "and kept tracked: it never reaches the terminal branch"
    for forbidden in ("cancel_status_proven_", "remove_offer", "fill_proof_deferrals_"):
        assert forbidden not in block, f"a shallow Dead neither remembers nor releases anything: {forbidden}"


def test_an_answer_that_settles_nothing_is_asked_again_for_confirmation_depth():
    """[#172's review] An Unknown under a cancel's status let the status stand
    at once for an offer not held -- also when it came from an answer that
    settled nothing: one that did not cover every maker coin, a record that
    could not be read, the wallet's silence.  A node catching up can still
    complete such an answer, and a take hidden under the status was then never
    asked about again.  Now such an offer stays tracked, cancel_pending, and is
    proven every heartbeat for confirmation_depth_blocks from the first such
    answer under that status; only then does the status stand.  A conclusive
    answer ends the run.  An Unknown with nothing to ask -- no readable maker
    coin, or no settlement coin or requested amount to look for
    (ProofLookup::NothingToAsk) -- still lets the status stand at once."""
    manager = _source(OFFER_MANAGER)
    prove = _function_body(manager, PROVE_ON_CHAIN)
    nothing = re.search(r"if \(names\.empty\(\)\) \{\s*lookup\s*= ProofLookup::NothingToAsk;", prove)
    assert nothing, "a record with no readable maker coin gives the proof nothing to ask"
    second = re.search(r"if \(ask_node \? settlements\.empty\(\) : amounts\.empty\(\)\) \{\s*"
                       r"lookup\s*= ProofLookup::NothingToAsk;", prove)
    assert second, "nor one that names no settlement coin or requested amount to look for"
    assert prove.count("ProofLookup::NothingToAsk") == 2
    detect = _function_body(manager, DETECT_FILLS)
    head = "for (const auto& rec : trade_records) {"
    loop = _block_after(detect[detect.rindex(head):], head)
    reproof = _block_after(loop, "if (reprove) {")
    ended = re.search(r"if \(reproof\.verdict != FillProof::Unknown\) \{\s*"
                      r"cancel_status_inconclusive_\.erase\(trade_id\);\s*\}", reproof)
    assert ended and ended.start() < reproof.index("if (reproof.verdict == FillProof::Settled) {"), (
        "a conclusive answer ends a run of inconclusive ones, before any verdict acts"
    )
    waits = re.search(r"\} else if \(reproof_lookup == ProofLookup::Refused\s*"
                      r"\|\| reproof_lookup == ProofLookup::Failed\) \{", reproof)
    answered = re.search(r"\} else if \(reproof_lookup == ProofLookup::Answered\) \{", reproof)
    assert waits and answered and waits.start() < answered.start(), (
        "a failed lookup waits first; only an answer opens the window"
    )
    open_index = answered.end() - 1
    window = reproof[open_index:_matching(reproof, open_index) + 1]
    opened = re.search(r"auto window = cancel_status_inconclusive_\.try_emplace\(\s*"
                       r"trade_id, InconclusiveSince\{status, current_block\}\)\.first;\s*"
                       r"if \(window->second\.status != status\) \{\s*"
                       r"window->second = InconclusiveSince\{status, current_block\};\s*\}", window)
    assert opened, "the window opens at the first such answer, and again when the status changes"
    within = re.search(r"if \(current_block < window->second\.block\s*"
                       r"\|\| current_block - window->second\.block\s*"
                       r"< strategy_cfg_\.confirmation_depth_blocks\) \{", window)
    assert within and opened.end() <= within.start(), "it lasts confirmation_depth_blocks"
    inside = window[within.end() - 1:_matching(window, within.end() - 1) + 1]
    assert re.match(r"\{\s*state_->mark_cancel_pending\(trade_id\);", inside), (
        "while it lasts, the offer is flagged cancel_pending"
    )
    assert re.search(r"continue;\s*\}$", inside), "...and asked again next heartbeat"
    assert "cancel_status_proven_" not in inside, "...remembering nothing"
    after = window[within.end() - 1 + len(inside):]
    assert re.match(r"\s*cancel_status_inconclusive_\.erase\(window\);\s*"
                    r"cancel_status_proven_\[trade_id\] = status;\s*\}$", after), (
        "only once it has passed does the status stand"
    )
    nothing_else = re.search(r"\} else \{\s*cancel_status_proven_\[trade_id\] = status;\s*\}\s*\}$", reproof)
    assert nothing_else and nothing_else.start() > answered.start(), (
        "nothing to ask: the status stands at once"
    )
    pruned = re.search(r"for \(auto it = cancel_status_inconclusive_\.begin\(\); "
                       r"it != cancel_status_inconclusive_\.end\(\);\) \{\s*"
                       r"it = pending_map\.count\(it->first\) \? std::next\(it\) "
                       r": cancel_status_inconclusive_\.erase\(it\);\s*\}", detect)
    assert pruned and pruned.start() < detect.rindex(head), "pruned before the loop, as the deferrals are"
    assert "cancel_status_inconclusive_.clear();" in _block_after(detect, "if (pending_offers.empty()) {")
    assert re.search(r"std::unordered_map<std::string, InconclusiveSince>\s+cancel_status_inconclusive_;",
                     _source(OFFER_MANAGER_HPP))


def test_a_local_cancel_stays_tracked_while_it_can_be_taken():
    """[review #171, round 15] A wallet CANCELLED with every maker coin unspent
    is a local cancel -- emergency_cancel's last resort, or one made in the
    wallet's own UI -- and the offer can still be taken.  In chia 2.7.4 the
    wallet watches no CANCELLED trade's coins (get_trades_by_coin skips them),
    so it would never report the take: only the fill proof can see it.
    detect_fills proved such an offer Live once and then closed it, and
    reconcile_offers closed it unproven.  Now it stays tracked, flagged
    cancel_pending, and is proven every heartbeat until the chain shows it
    taken or dead.  The one exception is an offer retire_expired_offers
    cancelled locally after proving it expired at depth: nothing can take
    it, so it closes on that verdict, which #157 needs."""
    manager = _source(OFFER_MANAGER)
    detect = _function_body(manager, DETECT_FILLS)
    head = "for (const auto& rec : trade_records) {"
    loop = _block_after(detect[detect.rindex(head):], head)
    reproof = _block_after(loop, "if (reprove) {")
    kept = re.search(r"\} else if \(status == trade_status::kCancelled\s*"
                     r"&& expiry_retired_\.count\(trade_id\) == 0U\) \{", reproof)
    assert kept, "a Live CANCELLED stays tracked, unless the expiry retire proved it expired"
    live = reproof.index("reproof.verdict == FillProof::Live")
    assert live < kept.start() < reproof.index("} else if (held) {"), (
        "on a Live answer only: a Dead one is remembered, and closes it"
    )
    open_index = kept.end() - 1
    block = reproof[open_index:_matching(reproof, open_index) + 1]
    assert re.match(r"\{\s*state_->mark_cancel_pending\(trade_id\);", block), (
        "flagged cancel_pending first, so nothing cancels it again"
    )
    assert re.search(r"continue;\s*\}$", block), (
        "and kept: it never reaches the terminal branch, which removes it"
    )
    assert "cancel_status_proven_" not in block and "remove_offer" not in block, (
        "nothing remembered, so it is proven again next heartbeat, and nothing removed"
    )
    # The expiry retire records what it proved, and nothing else does.
    retire = _function_body(manager, RETIRE_EXPIRED)
    local = retire.index("co_await cancel_offer_charged(po.offer_id, 0, /*secure=*/false);")
    recorded = re.search(r"state_->mark_cancel_pending\(po\.offer_id\);\s*"
                         r"expiry_retired_\.insert\(po\.offer_id\);\s*"
                         r"retired\.push_back\(po\.offer_id\);", retire)
    assert recorded and local < recorded.start(), (
        "recorded once its local cancel was accepted, with the retire itself"
    )
    assert manager.count("expiry_retired_.insert(") == 1, (
        "only the expiry retire, which proved the offer expired, records one"
    )
    pruned = re.search(r"for \(auto it = expiry_retired_\.begin\(\); it != expiry_retired_\.end\(\);\) \{\s*"
                       r"it = pending_map\.count\(\*it\) \? std::next\(it\) "
                       r": expiry_retired_\.erase\(it\);\s*\}", detect)
    assert pruned and pruned.start() < detect.rindex(head), "pruned before the loop, as the deferrals are"
    assert "expiry_retired_.clear();" in _block_after(detect, "if (pending_offers.empty()) {")
    assert re.search(r"std::unordered_set<std::string>\s+expiry_retired_;", _source(OFFER_MANAGER_HPP))
    # Reconciliation leaves a CANCELLED to detect_fills, in the scan and off it.
    reconcile = _function_body(manager, RECONCILE_OFFERS)
    scan = re.search(r"if \(status == trade_status::kCancelled\) \{\s*"
                     r"state_->mark_cancel_pending\(trade_id\);\s*"
                     r"\} else if \(status == trade_status::kFailed\) \{\s*"
                     r"state_->remove_offer\(trade_id\);", reconcile)
    assert scan, "in the scan: a CANCELLED is flagged and left to detect_fills, a FAILED removed"
    off_scan = re.search(r"if \(status == trade_status::kCancelled\) \{\s*"
                         r"state_->mark_cancel_pending\(offer_id\);", reconcile)
    assert off_scan, "off the scan: a CANCELLED is flagged and left to detect_fills"
    open_index = reconcile.index("{", off_scan.start())
    close_index = _matching(reconcile, open_index)
    left = reconcile[open_index:close_index + 1]
    assert re.search(r"continue;\s*\}$", left) and "remove_offer" not in left, (
        "...and never reaches the removal"
    )
    assert re.match(r"\s*if \(status != trade_status::kFailed\) \{", reconcile[close_index + 1:]), (
        "then only a FAILED goes on to be removed"
    )
    assert reconcile.count("state_->remove_offer(") == 2, "the two FAILED removals, and no other"


def test_a_wallet_cancelled_offer_is_unresolved_from_the_first_sight():
    """[review #171, round 20] A CANCELLED offer was flagged cancel_pending, and
    remembered as no cancel in flight (local_cancel_live_), only on a Live
    answer.  A lookup that failed, an answer that settled nothing, or a
    shallow Dead left it unflagged or unremembered, so a TTL or reprice path
    could cancel it again, or cancel_ids report it as a cancel in flight and
    let a shutdown retry drop it unproven.  Now every CANCELLED offer is
    flagged and remembered from the first sight of that status, before any
    proof, until the chain shows it taken or dead at depth -- except one the
    expiry retire cancelled, which closes on a Live answer."""
    manager = _source(OFFER_MANAGER)
    detect = _function_body(manager, DETECT_FILLS)
    head = "for (const auto& rec : trade_records) {"
    loop = _block_after(detect[detect.rindex(head):], head)
    pending = re.search(r"if \(status == trade_status::kPendingCancel\) \{\s*"
                        r"state_->mark_cancel_pending\(trade_id\);\s*\}", loop)
    mark = re.search(r"if \(status == trade_status::kCancelled "
                     r"&& expiry_retired_\.count\(trade_id\) == 0U\) \{\s*"
                     r"state_->mark_cancel_pending\(trade_id\);\s*"
                     r"local_cancel_live_\.insert\(trade_id\);\s*\}", loop)
    assert pending and mark, "a CANCELLED is flagged and remembered, as a PENDING_CANCEL is flagged"
    assert pending.end() <= mark.start() < loop.index("if (reprove) {"), (
        "before any proof, so no answer -- or the lack of one -- can leave it unmarked"
    )
    # Left only by a take or a Dead proof at depth (round 19), or with the
    # offer, when it leaves State.
    reproof = _block_after(loop, "if (reprove) {")
    assert reproof.count("local_cancel_live_.erase(") == 1
    assert re.search(r"if \(dead_at_depth \|\| reproof\.verdict == FillProof::Settled\) \{\s*"
                     r"local_cancel_live_\.erase\(trade_id\);\s*\}", reproof)
    assert detect.count("local_cancel_live_.erase(") == 2, "that erase, and the prune"


def test_a_dead_offer_left_pending_cancel_is_closed_at_depth():
    """[review #172] A PENDING_CANCEL offer proven Dead at depth was only
    remembered: its status stood, and the terminal branch removes only
    CANCELLED and FAILED, so the offer stayed tracked for as long as the
    wallet said PENDING_CANCEL -- for good when the cancel spends a coin that
    another spend already took (2026-09-21).  Now it is closed and reported
    dead_on_chain through handle_unproven_fill, as a CONFIRMED offer dead at
    depth is, and the record and both logs name the status the wallet
    reported."""
    manager = _source(OFFER_MANAGER)
    detect = _function_body(manager, DETECT_FILLS)
    head = "for (const auto& rec : trade_records) {"
    loop = _block_after(detect[detect.rindex(head):], head)
    reproof = _block_after(loop, "if (reprove) {")
    branch = re.search(r"\} else if \(dead_at_depth && status == trade_status::kPendingCancel\) \{", reproof)
    assert branch, "a PENDING_CANCEL dead at depth has a branch of its own"
    assert (reproof.index("if (reproof.verdict == FillProof::Settled) {") < branch.start()
            < reproof.index("} else if (reproof.verdict == FillProof::Live || dead_at_depth) {")), (
        "after a take is booked, and before the branch that would only remember it"
    )
    block = reproof[branch.end() - 1:_matching(reproof, branch.end() - 1) + 1]
    assert _call_args(block, "handle_unproven_fill") == [[
        "trade_id", "po", "reproof", "reproof_failure", "reproved_asked_node", "0U",
        "current_block", '"PENDING_CANCEL"']], "closed as a CONFIRMED offer dead at depth is"
    assert re.search(r"continue;\s*\}$", block), "and never reaches the terminal branch"
    assert "cancel_status_proven_" not in block
    # [round 21] ...and the call's summary line covers both statuses.
    assert re.search(r'"detect_fills: \{\} offer\(s\) the wallet reports CONFIRMED or "\s*'
                     r'"PENDING_CANCEL were proven never taken -- none booked as a fill"', detect), (
        "the summary of the call's dead offers names the PENDING_CANCEL ones too"
    )
    handler = _function_body(manager, HANDLE_UNPROVEN)
    assert "std::string_view wallet_status)" in handler, "the wallet's status is an argument"
    assert _call_args(handler, "last_dead_offers_.push_back") == [[
        "DeadOffer{trade_id, po.pair_name, proof.height, proof.coins, proof.unspent, "
        "proof.spent_together, std::string(wallet_status)}"]], "and goes with the dead offer"
    # [round 22] ...and so does the deferral warning: three lines in all.
    assert handler.count("the wallet reports {}, ") == 3
    assert "reports CONFIRMED" not in handler and "reported CONFIRMED" not in handler
    hpp = _source(OFFER_MANAGER_HPP)
    assert re.search(r'BlockHeight current_block,\s*std::string_view wallet_status = "CONFIRMED"\);', hpp), (
        "CONFIRMED unless a caller says otherwise"
    )
    assert re.search(r'std::string\s+wallet_status\{"CONFIRMED"\};', hpp)
    step = _function_body(_source(ENGINE), STEP_PROCESS_FILLS)
    writes = _block_after(step, WRITE_LOOP)
    assert "the wallet reported it {}, but " in writes and "dead.wallet_status," in writes, (
        "the engine's record names what the wallet reported"
    )


def test_only_the_wallets_missing_coin_refusal_is_one_offers():
    """[review #171, round 20] The refusal that is one offer's own was matched
    by "not found" anywhere in the text, which a refusal that is every
    offer's -- an unknown method's or endpoint's -- can say as well: it would
    skip the call-wide latch and repeat a doomed lookup for every offer.  Now
    only the wallet's own shape counts, the list of coin ids and then its
    end (gtest CoinLookupRefusal pins the behaviour)."""
    body = _block_after(_source(FILL_PROOF_HPP),
                        "constexpr bool coin_lookup_refusal_is_offer_local(std::string_view error) noexcept")
    assert 'constexpr std::string_view head = "Coin ID\'s: [";' in body
    assert 'constexpr std::string_view tail = "] not found.";' in body
    assert re.search(r"error\.find\(tail, at \+ head\.size\(\)\)", body), "the list, then its end"
    assert 'find("not found")' not in body


def test_a_malformed_stage_one_reply_is_a_failed_lookup():
    """[review #171, round 7] Both first-stage wrappers -- the node's and the
    wallet's get_coin_records_by_names -- throw on a reply without its
    coin_records list, as the two second-stage wrappers do.  An empty list
    read from a malformed reply was Unknown without tripping the latch, so
    every later CONFIRMED offer in the call asked again.

    [round 15] Still a failure, not a refusal: the wrappers throw a plain
    ChiaRPCError, which the ChiaRPCApplicationError catch of a refusal does
    not take."""
    rpc = _source(CHIA_RPC)
    for signature in ("ChiaFullNodeRPC::get_coin_records_by_names(", WALLET_COIN_RECORDS):
        body = _function_body(rpc, signature)
        refused = re.search(r"if \(listed == resp\.end\(\) \|\| !listed->is_array\(\)\) \{\s*"
                            r"throw ChiaRPCError\(", body)
        assert refused and refused.start() < body.index("records.push_back("), (
            f"{signature} must throw on a malformed reply before reading anything from it"
        )
    prove = _function_body(_source(OFFER_MANAGER), PROVE_ON_CHAIN)
    stage1 = prove.index("fill_proof_node_->get_coin_records_by_names(")
    caught = _block_after(prove[stage1:], "catch (const std::exception& e) {")
    assert "lookup = ProofLookup::Failed;" in caught, "a first-stage throw trips the latch"


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


WRITE_LOOP = "for (auto& w : pending_dead_writes_) {"


def test_the_engine_records_every_dead_offer_as_cancelled_dead_on_chain():
    step = _function_body(_source(ENGINE), STEP_PROCESS_FILLS)
    detect_at = step.index("co_await offer_mgr_->detect_fills(")
    head = "for (const auto& dead : offer_mgr_->last_dead_offers()) {"
    assert step.index(head) > detect_at, "the dead list describes the detect_fills call above it"
    assert "pending_dead_writes_.push_back(" in _block_after(step, head), (
        "every dead offer detect_fills reports is queued for its write"
    )
    assert step.index(WRITE_LOOP) > step.index(head)
    loop = _block_after(step, WRITE_LOOP)
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
        "the chain cannot say whose spend killed a dead offer, so a fee ticket "
        "for a cancel on it closes unheard"
    )


def test_a_failed_dead_offer_write_is_retried_and_bounded():
    """[review #171] detect_fills reports a dead offer once, having already
    stopped tracking it, so a write that fails must be retried -- and not for
    ever, the S25 buffer's rule."""
    step = _function_body(_source(ENGINE), STEP_PROCESS_FILLS)
    loop = _block_after(step, WRITE_LOOP)
    failed = _block_after(loop, "catch (const std::exception& ex) {")
    bound = re.search(r"if \(\+\+w\.failures >= kMaxTerminalPersistFailures\) \{", failed)
    assert bound, "the bound is exactly S25's: kMaxTerminalPersistFailures consecutive failures"
    gave_up = _block_after(failed, bound.group(0))
    assert "continue;" in gave_up and "dead_still_pending" not in gave_up, (
        "at the bound the entry is dropped"
    )
    assert "dead_still_pending.push_back(std::move(w));" in failed[bound.start() + len(gave_up):], (
        "below the bound a write that fails is re-queued"
    )
    not_ours = _block_after(loop, "catch (const OfferNotFound& nf) {")
    assert "dead_still_pending" not in not_ours, "an offer with no row is never retried"
    assert step.index("pending_dead_writes_ = std::move(dead_still_pending);") > step.index(WRITE_LOOP)


def test_detect_fills_describes_only_its_own_call():
    detect = _function_body(_source(OFFER_MANAGER), DETECT_FILLS)
    assert detect.index("last_dead_offers_.clear();") < detect.index("co_return"), (
        "the dead list must be cleared before detect_fills can return, or the "
        "engine records the same offers again"
    )


def test_a_wallet_cancelled_row_is_proven_before_boot_closes_it():
    """[review #171, round 13] At boot, startup_reconcile's DB leg counted a
    wallet CANCELLED as terminal, and the engine stamped the row cancelled
    and dropped it before restoring the book.  So a take that a cancel had
    overwritten before a restart never reached the round-12 proof, and was
    lost for good.  CANCELLED is now a bucket of its own.  The row is not
    stamped: it restores into State flagged cancel_pending, and detect_fills
    proves it on-chain the first time it polls it.  FAILED, which no cancel
    writes, is still stamped."""
    reconcile = _function_body(_source(OFFER_MANAGER), STARTUP_RECONCILE)
    buckets = re.search(r"if \(status == trade_status::kFailed\) \{\s*db_leg_\.terminal\.push_back\(id\);\s*"
                        r"\} else if \(status == trade_status::kCancelled\) \{\s*"
                        r"db_leg_\.cancelled_unproven\.push_back\(id\);\s*\}", reconcile)
    assert buckets, "a wallet CANCELLED must not be terminal at boot"
    assert reconcile.count("db_leg_.terminal.push_back(") == 1
    leg = _block_after(_source(OFFER_MANAGER_HPP), "struct StartupDbLeg {")
    assert "std::vector<std::string> cancelled_unproven;" in leg
    assert "cancelled_unproven.size()" in _block_after(leg, "std::size_t total() const noexcept")

    engine = _function_body(_source(ENGINE), POLL_LOOP)
    stamp = _block_after(engine, "for (const auto& oid : leg.terminal) {")
    assert "cancelled_unproven" not in stamp, "a CANCELLED row must not be stamped"
    carried = re.search(r"if \(!leg\.cancelled_unproven\.empty\(\)\) \{\s*"
                        r"cancelled_unproven = leg\.cancelled_unproven;", engine)
    assert carried, "the ids are carried out to the restore"
    assert engine.count("known_ids.erase(") == 1, "only a stamped row stays out of the restore"
    restore = engine.index("const PendingOffer po = pending_offer_from_db(rec);")
    flagged = re.search(r"for \(const auto& oid : cancelled_unproven\) \{\s*"
                        r"state_->mark_cancel_pending\(oid\);\s*\}", engine)
    assert flagged and flagged.start() > restore, "flagged cancel_pending once restored"
