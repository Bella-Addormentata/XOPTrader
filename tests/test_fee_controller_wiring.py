"""[S67 2026-09-20] Fee controller wiring in the C++ engine, as a source scan.

LINT-CLASS GUARD, disclosed as such.  These tests read cpp/src/engine.cpp,
cpp/src/execution/offer_manager.cpp and two headers as TEXT.  They pin call-site
WIRING that no C++ unit test reaches -- Engine and OfferManager are not
constructible in xop_tests -- while every DECISION is pinned by gtest
(cpp/tests/test_fee_controller.cpp, test_node_requests.cpp, test_config.cpp).
A pass here says the engine still CALLS those decisions at every site; it says
nothing about runtime behaviour.

What they guard.  The fee used to be an open loop: clamp(node estimate for a
plain XCH send).  The controller closes it, and a closed loop is only as good
as its wiring: a fee site that names no action class pays the wrong cost
model, and a feedback site that loses its hook silently turns the loop back
into an open one -- nothing fails, the fee just stops moving.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "cpp" / "src" / "engine.cpp"
ENGINE_HPP = REPO / "cpp" / "include" / "xop" / "engine.hpp"
OFFER_MANAGER = REPO / "cpp" / "src" / "execution" / "offer_manager.cpp"
FEE_TRACKER_HPP = REPO / "cpp" / "include" / "xop" / "strategy" / "fee_tracker.hpp"
CHIA_RPC = REPO / "cpp" / "src" / "rpc" / "chia_rpc.cpp"

ACTION_CLASS = re.compile(
    r"strategy::fee::ActionClass::(OfferAttached|CancelXch|CancelCat|Take)\b")

# Every get_recommended_fee call site in engine.cpp, by the class it must name.
# Step 8 asks three times (the offer-attached fee, then the two cancel classes
# it hands to OfferManager), five take sites ask for Take, and the #157 cancel
# escalation asks for CancelCat.
EXPECTED_CLASSES = {
    "OfferAttached": 1,
    "CancelXch": 1,
    "CancelCat": 2,
    "Take": 5,
}
CALL_SITE_COUNT = 9


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


def _call_arguments(text: str, callee: str) -> list[str]:
    """The argument text of every `<callee>(` in comment-stripped `text`."""
    results: list[str] = []
    for match in re.finditer(re.escape(callee) + r"\s*\(", text):
        i = match.end() - 1
        start = i + 1
        depth = 0
        quote = ""
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
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    results.append(text[start:i])
                    break
            i += 1
    return results


def _function_body(text: str, signature: str) -> str:
    """The text of the function whose definition starts with `signature`, up to
    the closing brace at column 0."""
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def _engine() -> str:
    return _strip_line_comments(_read(ENGINE))


# ---------------------------------------------------------------------------
# Every fee site names an action class
# ---------------------------------------------------------------------------

def test_every_get_recommended_fee_call_site_passes_an_action_class():
    calls = _call_arguments(_engine(), "get_recommended_fee")
    assert len(calls) == CALL_SITE_COUNT, (
        "expected %d get_recommended_fee call sites in engine.cpp, found %d -- "
        "the scan must not pass vacuously, and a new fee site needs a class "
        "chosen on purpose" % (CALL_SITE_COUNT, len(calls)))
    missing = [args for args in calls if not ACTION_CLASS.search(args)]
    assert not missing, "fee site(s) without an ActionClass: %r" % missing
    seen: dict[str, int] = {}
    for args in calls:
        names = ACTION_CLASS.findall(args)
        assert len(names) == 1, "one class per call site: %r" % args
        seen[names[0]] = seen.get(names[0], 0) + 1
    assert seen == EXPECTED_CLASSES, seen


def test_the_class_parameter_has_no_default():
    """The compile-time half of the guard above: with a default, a site that
    forgets the class would still build and pay the offer-attached fee."""
    header = _strip_line_comments(_read(FEE_TRACKER_HPP))
    decl = _call_arguments(header, "std::uint64_t get_recommended_fee")
    assert len(decl) == 1, decl
    assert "strategy::fee::ActionClass action" in decl[0]
    assert "=" not in decl[0], "get_recommended_fee's class must not be defaulted: %r" % decl[0]


def test_take_sites_pay_the_take_class_and_open_a_ticket():
    engine = _engine()
    takes = len(re.findall(r"fee_feedback_track_take\s*\(", engine))
    # One definition plus the four take sites that go through the FeeTracker
    # (9c, 9d, 9e, 9f).  The XCH-recovery take pays min_fee by design and is
    # not tracked.
    assert takes == 5, takes
    for site in ("Step 9c", "Step 9d", "Step 9e", "Step 9f"):
        assert site in engine


def test_step8_hands_offer_manager_class_aware_cancel_fees_only_when_active():
    body = _function_body(_engine(), "asio::awaitable<void> Engine::step_manage_offers(")
    # [review #163] class_fees_active(), NOT controller_active(): with only
    # fees.cost_aware_estimate on, a CAT cancel must still pay its own class.
    gate = body.index("if (fee_tracker_->class_fees_active()) {")
    assert "fee_tracker_->controller_active()" not in body[gate - 400:gate + 900]
    set_at = body.index("offer_mgr_->set_cancel_fees(", gate)
    clear_at = body.index("offer_mgr_->clear_cancel_fees()", set_at)
    assert gate < set_at < clear_at
    between = body[gate:clear_at]
    assert "ActionClass::CancelXch" in between and "ActionClass::CancelCat" in between
    # The XCH fee is the FIRST argument, the CAT fee the second.
    assert between.index("ActionClass::CancelXch") < between.index("ActionClass::CancelCat")
    # and it happens right after the legacy single fee is set, before any cancel.
    assert body.index("offer_mgr_->set_dynamic_fee(recommended_fee)") < gate
    assert gate < body.index("selective_cancel(")


def test_offer_manager_cancels_pay_cancel_fee_for_not_the_single_fee():
    text = _strip_line_comments(_read(OFFER_MANAGER))
    calls = _call_arguments(text, "cancel_offer_charged")
    per_offer = [a for a in calls if "cancel_fee_for(" in a]
    assert len(per_offer) == 6, (
        "expected the six per-offer cancel sites to pay cancel_fee_for(); found %d: %r"
        % (len(per_offer), per_offer))
    # No per-offer cancel may still pay the single dynamic fee directly.  (The
    # bulk cancel_offers_charged sweep keeps it: one batch fee for a whole book.)
    stale = [a for a in calls if re.search(r"\bcurrent_fee_mojos_\b", a)]
    assert not stale, stale
    # Inert without the override: the first statement returns the legacy fee.
    body = _function_body(text, "std::uint64_t OfferManager::cancel_fee_for(")
    assert re.search(r"if\s*\(\s*!cancel_fees_active_\s*\)\s*\{\s*return current_fee_mojos_;", body)


# ---------------------------------------------------------------------------
# The feedback hooks exist where the evidence is
# ---------------------------------------------------------------------------

def test_cancel_verdict_site_feeds_the_controller():
    engine = _engine()
    verdict = engine.index('"wallet reported terminal"')
    hook = engine.index("fee_feedback_on_cancel_verdict(", verdict)
    # Directly after the write, inside the same try: nothing but the call's own
    # arguments in between.
    assert hook - verdict < 200, hook - verdict
    args = _call_arguments(engine[hook:hook + 400], "fee_feedback_on_cancel_verdict")[0]
    assert "t.offer_id" in args and "t.observed_block" in args, args


def test_pending_change_site_feeds_the_controller_on_an_edge():
    body = _function_body(_engine(), "asio::awaitable<void> Engine::step_manage_offers(")
    bump = body.index("++consecutive_pending_blocks_;")
    hook = body.index("strategy::fee::Signal::PendingChangeStuck", bump)
    assert hook - bump < 600, hook - bump
    between = body[bump:hook]
    assert "consecutive_pending_blocks_ == kForceDeletePendingBlocks / 2" in between, (
        "the pending_change signal must fire on the half-way EDGE, once per run")


def test_force_delete_site_feeds_the_controller_before_the_counter_resets():
    body = _function_body(_engine(), "asio::awaitable<void> Engine::step_manage_offers(")
    wipe = body.index("delete_unconfirmed_transactions(pw)")
    hook = body.index("strategy::fee::Signal::ForceDelete", wipe)
    reset = body.index("consecutive_pending_blocks_ = 0;", wipe)
    assert wipe < hook < reset


def test_prune_site_reads_the_sent_to_refusals_it_already_holds():
    body = _function_body(_engine(), "asio::awaitable<void> Engine::step_manage_offers(")
    prune = body.index("offer_mgr_->prune_stuck_transactions(")
    taken = body.index("offer_mgr_->take_fee_rejections_seen()", prune)
    hook = body.index("strategy::fee::Signal::MempoolRejected", taken)
    assert prune < taken < hook and hook - prune < 900
    om = _strip_line_comments(_read(OFFER_MANAGER))
    pruner = _function_body(om, "asio::awaitable<int> OfferManager::prune_stuck_transactions(")
    assert "execution::latest_sent_to_is_fee_rejection(tx)" in pruner
    # No RPC was added to read it: still exactly one get_transactions call and
    # one wallet-wide delete in the pruner.
    assert len(re.findall(r"wallet_->get_transactions\(", pruner)) == 1
    assert len(re.findall(r"wallet_->\w+\(", pruner)) == 2


def test_sweep_runs_every_heartbeat_beside_the_cancel_escalation():
    engine = _engine()
    escalate = engine.index("co_await escalate_stuck_cancels(block_height)")
    sweep = engine.index("co_await fee_feedback_sweep(block_height)")
    recovery_gate = engine.index("if (xch_recovery_mode_) {", escalate)
    # After the escalation, above the Step 7/8 gate chain -- "not trading" is no
    # reason to stop hearing that a cancel is stuck.
    assert escalate < sweep < recovery_gate


def test_every_glue_function_is_inert_with_the_controller_off():
    engine = _engine()
    for signature in (
        "void Engine::fee_feedback_note(",
        "void Engine::fee_feedback_signal(",
        "void Engine::fee_feedback_track_take(",
        "void Engine::fee_feedback_track_cancel(",
        "void Engine::fee_feedback_on_cancel_verdict(",
        "asio::awaitable<void> Engine::fee_feedback_sweep(",
    ):
        body = _function_body(engine, signature)
        head = body[:body.index("{") + 400]
        assert "!fee_tracker_->controller_active()" in head, signature
    # cancel_fees_paid keeps the old product unless fees depend on the class.
    paid = _function_body(engine, "std::uint64_t Engine::cancel_fees_paid(")
    assert "!fee_tracker_->class_fees_active()" in paid
    assert "static_cast<std::uint64_t>(ids.size()) * legacy_fee" in paid
    # The sweep pays nothing and cancels nothing: its only wallet call is a read.
    sweep = _function_body(engine, "asio::awaitable<void> Engine::fee_feedback_sweep(")
    assert re.findall(r"wallet_->(\w+)\(", sweep) == ["transport_counters", "get_offer"]
    assert not re.search(r"offer_mgr_->\w+\(", sweep), "the sweep calls nothing on OfferManager"


def test_cancel_tickets_open_at_the_accepted_rpc_with_the_fee_really_paid():
    """[review #163] A ticket opened a heartbeat later, from the CURRENT policy,
    missed every cancel that confirmed inside that heartbeat and misattributed
    any whose fee was not the policy's (an emergency tier, a zero-fee retry, an
    escalation).  It is opened by OfferManager's cancel observer instead."""
    om = _strip_line_comments(_read(OFFER_MANAGER))
    charged = _function_body(om, "asio::awaitable<json> OfferManager::cancel_offer_charged(")
    rpc = charged.index("co_await wallet_->cancel_offer(trade_id, fee, secure)")
    seen = charged.index("cancel_observer_(trade_id, fee);", rpc)
    assert rpc < seen, "only AFTER the wallet accepted it: a refusal throws past the observer"
    # Exactly ONE call.  A second one ahead of the RPC would ticket a cancel the
    # wallet then refuses (a mutation that added one survived the check above).
    assert len(re.findall(r"cancel_observer_\s*\(", charged)) == 1
    assert re.search(r"if\s*\(secure\s*&&\s*cancel_observer_\)\s*\{\s*cancel_observer_\(trade_id, fee\);",
                     charged), "a local-only cancel spends nothing on chain: no ticket"
    # ... and every per-offer cancel in OfferManager goes through that choke point.
    assert len(re.findall(r"wallet_->cancel_offer\(", om)) == 1

    engine = _engine()
    install = engine.index("offer_mgr_->set_cancel_observer(")
    assert "fee_feedback_track_cancel(id, fee);" in engine[install:install + 300]
    assert engine.index("offer_mgr_ = std::make_unique<execution::OfferManager>(") < install

    track = _function_body(engine, "void Engine::fee_feedback_track_cancel(")
    # The ticket carries the fee the observer was GIVEN, never a policy lookup.
    args = _call_arguments(track, "fee_tracker_->make_ticket")
    assert len(args) == 1 and re.search(r",\s*fee,\s*last_block_\.load\(", args[0]), args
    assert "cancel_fee_for(" not in track and "get_recommended_fee(" not in track
    assert "insert_or_assign(offer_id" in track, "a re-cancel replaces the ticket"

    # The sweep opens NO cancel ticket: a cancel adopted at boot (fee unknown)
    # therefore never has one.
    sweep = _function_body(engine, "asio::awaitable<void> Engine::fee_feedback_sweep(")
    assert "make_ticket" not in sweep and "fee_tickets_.emplace" not in sweep


def test_only_a_wallet_verified_cancelled_reaches_the_controller():
    """[review #163] recheck_terminal answers StillTerminal for CANCELLED and
    FAILED alike; only the first says a cancel spend confirmed."""
    om = _strip_line_comments(_read(OFFER_MANAGER))
    recheck = _function_body(om, "OfferManager::recheck_terminal(")
    assert re.search(r"\*wallet_cancelled_out\s*=\s*false;", recheck), "cleared on entry"
    assert re.search(r"\*wallet_cancelled_out\s*=\s*\(status == trade_status::kCancelled\);", recheck)
    assert len(re.findall(r"\*wallet_cancelled_out\s*=", recheck)) == 2

    engine = _engine()
    verdict = engine.index('"wallet reported terminal"')
    flag = engine.rindex("bool wallet_says_cancelled = false;", 0, verdict)
    call = engine.index("&wallet_says_cancelled);", flag)
    hook = engine.index("fee_feedback_on_cancel_verdict(", verdict)
    assert flag < call < verdict < hook
    args = _call_arguments(engine[hook:hook + 400], "fee_feedback_on_cancel_verdict")[0]
    assert args.rstrip().endswith("wallet_says_cancelled"), args
    body = _function_body(engine, "void Engine::fee_feedback_on_cancel_verdict(")
    assert "strategy::fee::observation_for_cancel_verdict(" in body
    assert re.search(r"if\s*\(v\.has\)\s*\{\s*fee_feedback_note\(fee_tracker_->observe\(v\.observation\)", body)
    assert "Signal::Confirmed" not in body, "the decision lives in the pure header"


def test_step8_tells_the_budget_how_many_offers_one_attached_fee_covers():
    """[review #163] One attached fee is asked for and then attached to every
    tier posted; shaped for one offer, a ladder could spend the cancel reserve."""
    body = _function_body(_engine(), "asio::awaitable<void> Engine::step_manage_offers(")
    batch = body.index("fee_tracker_->set_attached_batch(")
    ask = body.index("strategy::fee::ActionClass::OfferAttached")
    assert batch < ask, "the batch must be set BEFORE the attached fee is asked for"
    between = body[body.rindex("std::size_t may_post = 0;", 0, batch):batch]
    assert "cycle_" in between and "ladder.size()" in between and "quote_valid" in between
    assert len(re.findall(r"set_attached_batch\(", _engine())) == 1


# ---------------------------------------------------------------------------
# The estimate fix is gated, and adds no node call
# ---------------------------------------------------------------------------

def test_step1_asks_for_a_rate_only_when_wanted_and_keeps_the_wallet_only_gate():
    body = _function_body(_engine(), "asio::awaitable<void> Engine::step_update_market_state(")
    gate = body.index("&& !wallet_only_mode_")
    wants = body.index("fee_tracker_->wants_rate_estimate()", gate)
    rate = body.index("full_node_->get_fee_rate_estimate(", wants)
    legacy = body.index("full_node_->get_fee_estimate(", rate)
    assert gate < wants < rate < legacy
    # Exactly one of each, and the legacy call sits in the else branch.
    assert len(re.findall(r"full_node_->get_fee_rate_estimate\(", body)) == 1
    assert len(re.findall(r"full_node_->get_fee_estimate\(", body)) == 1
    assert re.search(r"\}\s*else\s*\{\s*auto est = co_await full_node_->get_fee_estimate\(", body)
    # The admission floor comes from the blockchain state already fetched.
    assert "full_node_->last_mempool_state()" in body
    assert "get_blockchain_state" not in body


def test_node_rpc_builds_both_payloads_from_the_pinned_helpers():
    rpc = _strip_line_comments(_read(CHIA_RPC))
    legacy = _function_body(rpc, "ChiaFullNodeRPC::get_fee_estimate(")
    assert "make_fee_estimate_request_legacy(target_time_seconds)" in legacy
    assert "send_xch_transaction" not in legacy, "the payload lives in node_requests.hpp"
    rate = _function_body(rpc, "ChiaFullNodeRPC::get_fee_rate_estimate(")
    assert "make_fee_estimate_request(target_time_seconds, kFeeEstimateReferenceCost)" in rate
    assert "parse_fee_estimate(resp, kFeeEstimateReferenceCost)" in rate
    height = _function_body(rpc, "asio::awaitable<std::int64_t> ChiaFullNodeRPC::get_block_height()")
    assert "last_mempool_state_ = node_mempool_from_blockchain_state(resp);" in height
    assert len(re.findall(r"rpc_post\(", height)) == 1, "no second RPC for the mempool state"
