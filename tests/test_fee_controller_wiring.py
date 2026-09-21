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


def _nth_argument(args: str, n: int) -> str:
    """The nth top-level argument of a captured argument list.  Splits on commas
    at paren/bracket depth 0, so `static_cast<std::uint64_t>(x)` stays whole."""
    depth = 0
    parts = []
    current = []
    for ch in args:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(current))
            current = []
            continue
        current.append(ch)
    parts.append("".join(current))
    return parts[n].strip() if n < len(parts) else ""


# [review #163 r4] The fee expression every cancel_offer_charged site is allowed
# to pass, and why.  An ALLOWLIST, not a denylist: the form before r4 counted
# `cancel_fee_for(` and rejected `current_fee_mojos_`, so a NEW site with a
# hardcoded non-zero fee -- say `cancel_offer_charged(id, 15000000, true)` --
# was neither token and passed silently while paying a fee no controller and no
# config could move.
#
# [review #163 r5] ... and r4's allowlist was matched by SUBSTRING, with the
# bare three-letter entry `fee` in it.  That one entry degenerated the whole
# list: `hardcoded_fee`, `wrong_fee` and `fee_for_unknown_class()` all contain
# it, so all three passed.  The tightening did catch the exact mutation the
# integration check found -- a bare `15000000` contains none of the tokens --
# but it narrowed the hole rather than closing it.  Matching is now EXACT, and
# the one genuinely structural case has its own rule below.
ALLOWED_CANCEL_FEES = {
    # [S31] The dead man's switch pays a fixed policy fee on purpose: the
    # watchdog has given up managing the book and must not consult a loop.
    "xop::risk::watchdog_cancel().fee_mojos": "the dead man's switch policy fee",
    # [S14/#157] The escalation's own fee, itself derived from
    # get_recommended_fee(CancelCat) -- pinned by the call-site scan above.
    "static_cast<std::uint64_t>(attempt_fee)": "the #157 escalation fee",
    # The definition and the single internal forwarder (recancel_secure).
    "std::uint64_t fee": "the function's own parameter, i.e. the definition",
    "fee": "forwarded unchanged by the one internal caller",
    # An explicit literal 0: a genuinely free retire pays nothing, and "free"
    # is a decision a reader can check at a glance.  Any OTHER literal is a
    # hardcoded fee and fails.
    "0": "an explicit free retire",
}

# The one STRUCTURAL exemption: the per-offer class-aware fee, whose argument
# is whatever local holds the offer id at that site.  Nothing may follow the
# closing paren, so `cancel_fee_for(id) + 1` is not this rule.
CANCEL_FEE_FOR = re.compile(r"cancel_fee_for\([^()]*\)\Z")


def _cancel_fee_allowed(fee_arg: str) -> bool:
    """Is `fee_arg` (whitespace-collapsed) a fee a cancel site may pay?"""
    return fee_arg in ALLOWED_CANCEL_FEES or bool(CANCEL_FEE_FOR.match(fee_arg))


def test_the_cancel_fee_allowlist_actually_rejects_an_invented_fee():
    """[review #163 r5] The scan below is worth exactly as much as this: a
    classifier that accepts everything pins nothing.  The three names here are
    the counter-examples the reviewer gave against the r4 substring form, plus
    the hardcoded literal r4 was written to catch and the stale member it
    replaced."""
    for bad in (
        "hardcoded_fee",                # substring-matched `fee`
        "wrong_fee",                    # substring-matched `fee`
        "fee_for_unknown_class()",      # substring-matched `fee`
        "some_fee",
        "15000000",                     # the integration check's M7 mutation
        "15'000'000ULL",
        "current_fee_mojos_",           # the member the per-offer fee replaced
        "cancel_fee_for(id) + 1",       # not the structural rule
        "cancel_fee_for(id) * 2",
        "std::max(cancel_fee_for(id), 15000000ULL)",
        "fee + 1",
        "fee * 2",
        "0 + 1",
        "1",
    ):
        assert not _cancel_fee_allowed(bad), bad
    for good in (
        "cancel_fee_for(po.offer_id)",
        "cancel_fee_for(oid)",
        "cancel_fee_for(offer_id)",
        "cancel_fee_for(wo->trade_id)",
        "xop::risk::watchdog_cancel().fee_mojos",
        "static_cast<std::uint64_t>(attempt_fee)",
        "std::uint64_t fee",
        "fee",
        "0",
    ):
        assert _cancel_fee_allowed(good), good


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

    # [review #163 r4] And nothing else may invent a fee.  Every site's SECOND
    # argument must be an allowed expression -- [r5] by EQUALITY, or by the one
    # structural cancel_fee_for(...) rule.
    unknown = [
        fee_arg for fee_arg in
        (" ".join(_nth_argument(args, 1).split()) for args in calls)
        if not _cancel_fee_allowed(fee_arg)
    ]
    assert not unknown, (
        "cancel_offer_charged sites paying a fee that is neither cancel_fee_for(), an "
        "explicit 0, nor a documented policy fee: %r. A hardcoded fee cannot be moved by "
        "the controller, by fees.min_fee_mojos or by the operator -- use cancel_fee_for(), "
        "or add the new expression to ALLOWED_CANCEL_FEES with the reason it is exempt."
        % (unknown,))
    # Every allowlist entry must still be USED: a stale exemption is a hole
    # nobody is looking at.
    used = {" ".join(_nth_argument(args, 1).split()) for args in calls}
    unused = sorted(set(ALLOWED_CANCEL_FEES) - used)
    assert not unused, (
        "allowlist entries no cancel site pays any more -- delete them: %r" % (unused,))
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


def test_every_ticket_has_an_unconditional_age_cap_not_just_takes():
    """[review #163 r4] A cancel STRANDED by an exhausted #157 escalation sits
    `cancel_pending` in State for ever, so it never leaves `cancels_in_flight`,
    `awaiting_verdict` is never set and `verdict_expired` can never fire.  The
    sweep must therefore drop tickets on AGE, for every class, before it looks
    at anything else -- if the check sits behind an `is_take` branch, or after
    the verdict bookkeeping, the stranded cancel speaks once per height for the
    life of the process and fails every probe below the fee it paid."""
    engine = _strip_line_comments(_engine())
    body = _function_body(engine, "asio::awaitable<void> Engine::fee_feedback_sweep(")
    loop = body.index("for (auto it = fee_tickets_.begin();")
    abandoned = body.index("strategy::fee::ticket_abandoned(", loop)
    # First thing in the loop: before the is_take split and before the
    # awaiting_verdict bookkeeping, both of which a stranded cancel escapes.
    is_take = body.index("const bool is_take =", loop)
    verdict = body.index("strategy::fee::verdict_expired(", loop)
    assert abandoned < is_take, "the age cap must not sit behind the take/cancel split"
    assert abandoned < verdict, "the age cap must not sit behind the verdict rule"
    # ... and it must actually erase, not merely be evaluated.
    tail = body[abandoned:abandoned + 260]
    assert "fee_tickets_.erase(it)" in tail and "continue;" in tail, tail
    # The old take-only form must be gone: no age comparison may be reachable
    # only for takes.
    assert "strategy::fee::kVerdictTtlBlocks" not in body, (
        "the sweep compares an age against kVerdictTtlBlocks directly; that was the "
        "take-only rule ticket_abandoned replaced")


def test_a_confirmed_take_is_aged_from_its_confirmation_height():
    """[review #163 r5] The sweep polls ONE take per heartbeat, oldest first
    (the selection sits inside `if (is_take)`, so only another TAKE can delay
    it -- cancels are serviced in the same pass and never contend for the
    slot).  `o.blocks` was set once, in the prologue above the status switch,
    as the age at THIS heartbeat: a delayed poll then reported an on-time
    confirmation as late, which raises the fee or fails a probe that was
    right.  The CANCEL verdict has measured from the height the spend landed
    at since r1 (observation_for_cancel_verdict); the take path kept the old
    shape.  It is set PER BRANCH now."""
    body = _function_body(_engine(), "asio::awaitable<void> Engine::fee_feedback_sweep(")
    poll = body.index("co_await wallet_->get_offer(take_to_poll")
    tail = body[poll:]

    # Exactly two assignments, one per branch that speaks -- NOT one shared
    # prologue assignment above the switch.
    blocks = [m.start() for m in re.finditer(r"\bo\.blocks\s*=", tail)]
    assert len(blocks) == 2, (
        "o.blocks must be set inside each branch that observes, never once above the "
        "status switch: found %d assignment(s)" % len(blocks))
    confirmed = tail.index("strategy::fee::Signal::Confirmed")
    pending = tail.index("strategy::fee::Signal::Pending", confirmed)
    assert confirmed < blocks[0] < pending < blocks[1], (confirmed, blocks, pending)

    # The confirmed one measures from the record's own height, through the
    # helper that clamps a height regression to 0.
    conf_stmt = tail[blocks[0]:tail.index(";", blocks[0])]
    assert "strategy::fee::confirmation_delay(" in conf_stmt, conf_stmt
    assert "execution::confirmed_height_from_record(record" in conf_stmt, conf_stmt
    assert "ticket_age(" not in conf_stmt, (
        "a confirmed take must not be aged to the heartbeat that noticed: %r" % conf_stmt)
    # ... falling back to the heartbeat only when the record states no height.
    assert re.search(r"confirmed_height_from_record\(record,\s*block\)", conf_stmt), conf_stmt

    # A still-pending take has no confirmation height, so its censored
    # observation IS the age at this heartbeat.
    pend_stmt = tail[blocks[1]:tail.index(";", blocks[1])]
    assert "strategy::fee::ticket_age(" in pend_stmt, pend_stmt
    assert "confirmation_delay" not in pend_stmt, pend_stmt

    # The premise: one take per heartbeat, and the selection is take-only.
    assert len(re.findall(r"wallet_->get_offer\(", body)) == 1
    take_only = body.index("if (is_take) {")
    chosen = re.search(r"take_to_poll\s*=\s*it->first", body)
    assert chosen is not None and take_only < chosen.start()


def test_an_over_budget_quote_becomes_an_episode_only_at_an_accepted_spend():
    """[review #163 r5] Step 8 calls get_recommended_fee for both cancel
    classes every heartbeat BEFORE any cancellation, and a take fee is
    computed during candidate evaluation.  Latching the overrun inside the
    quote queued FeeBudgetUnfunded and logged "PAYING IT ANYWAY" on heartbeats
    where no wallet RPC was sent at all."""
    tracker = _strip_line_comments(_read(REPO / "cpp" / "src" / "strategy" / "fee_tracker.cpp"))
    priced = _function_body(tracker, "std::uint64_t FeeTracker::controller_fee(")
    # The quote records the would-be overrun and changes nothing else.
    assert "pending_unfunded_[static_cast<std::size_t>(action)]" in priced
    assert "budget_unfunded_" not in priced, (
        "controller_fee must not latch the unfunded episode: it prices, it does not spend")
    assert "unfunded_alert_pending_" not in priced
    assert "PAYING IT ANYWAY" not in priced
    # The BOUND episode is a different thing and stays here: it is a fact
    # about the fee this call RETURNS (the budget could not fund an attached
    # fee).  [review #163 r8] And it is latched on what the BUDGET GRANTED, not
    # on `bound` -- which is false whenever the attached fee is already pinned
    # at min_fee_mojos, so an exhausted budget was silent on the whole bottom
    # of the level band and an open episode was cleared from an empty window.
    assert "budget_bound_" in priced
    assert "budgeted.allowance < desired" in priced, (
        "the bound episode must latch on what the budget granted, not on "
        "BudgetedFee::bound -- see review #163 r8 F1")
    assert "budgeted.bound" not in priced, (
        "`bound` says the fee was lowered; it is false at the min_fee pin "
        "however empty the window is")

    spent = _function_body(tracker, "void FeeTracker::note_priority_spend(")
    assert "unfunded_alert_pending_ = true;" in spent
    assert "budget_unfunded_        = true;" in spent
    assert "budget_unfunded_ = false;" in spent, "and this is where the episode ends"
    # [review #163 r8] ... and it ends only for a spend that FITS THE HEADROOM.
    # The clear branch used to be the bare negation of the latch above.
    assert "fee_paid_mojos > quote.headroom" in spent, (
        "the unfunded episode must end on the headroom the quote measured, "
        "not merely on a fee below that quote -- see review #163 r8 F2")
    # Per class: Step 8 quotes CancelXch then CancelCat in the same heartbeat.
    assert "pending_unfunded_[static_cast<std::size_t>(action)]" in spent
    # An attached fee is never a priority spend.
    assert "strategy::fee::is_priority(action)" in spent

    engine = _engine()
    # ... and the engine promotes it at the two places a spend becomes real.
    notes = _call_arguments(engine, "fee_tracker_->note_priority_spend")
    assert len(notes) == 2, notes
    take = _function_body(engine, "void Engine::fee_feedback_track_take(")
    cancel = _function_body(engine, "void Engine::fee_feedback_track_cancel(")
    assert "fee_tracker_->note_priority_spend(strategy::fee::ActionClass::Take, fee);" in take
    assert "fee_tracker_->note_priority_spend(cls, fee);" in cancel
    # Ahead of every ticket guard: the mojos are committed whether or not a
    # ticket can be opened for them.
    assert take.index("note_priority_spend") < take.index("fee_tickets_.emplace(")
    assert cancel.index("note_priority_spend") < cancel.index("if (fee_now_block_ == 0) {")
    assert cancel.index("note_priority_spend") < cancel.index(
        "fee_tickets_.size() >= strategy::fee::kMaxTickets")
    # The cancel's class is decided once and used for both.
    assert cancel.index("strategy::fee::cancel_class(") < cancel.index("note_priority_spend")


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
    # [review #163 r6] The product is now computed in two statements so it can
    # saturate, so this pins the SHAPE (count x legacy_fee, one fee per cancel,
    # no per-class lookup) rather than one literal expression -- and pins the
    # overflow guard beside it, since an unguarded product wraps and under-books
    # the window.
    paid = _function_body(engine, "std::uint64_t Engine::cancel_fees_paid(")
    assert "!fee_tracker_->class_fees_active()" in paid
    legacy = paid[paid.index("class_fees_active"):paid.index("std::uint64_t total")]
    assert "static_cast<std::uint64_t>(ids.size())" in legacy
    assert "* legacy_fee" in legacy
    assert "cancel_fee_for" not in legacy, "the legacy branch must not price per class"
    assert "std::numeric_limits<std::uint64_t>::max() / n" in legacy, (
        "the legacy count x fee product must saturate, not wrap"
    )
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
    assert len(args) == 1 and re.search(r",\s*fee,\s*fee_now_block_\s*$", args[0]), args
    assert "cancel_fee_for(" not in track and "get_recommended_fee(" not in track
    assert "insert_or_assign(offer_id" in track, "a re-cancel replaces the ticket"

    # The sweep opens NO cancel ticket: a cancel adopted at boot (fee unknown)
    # therefore never has one.
    sweep = _function_body(engine, "asio::awaitable<void> Engine::fee_feedback_sweep(")
    assert "make_ticket" not in sweep and "fee_tickets_.emplace" not in sweep


def test_cancel_tickets_are_opened_at_the_cycle_height_never_at_last_block():
    """[review #163 r2] last_block_ is stored only when a cycle ENDS, so during
    cycle N it reads N-1 and before the first cycle ends it reads 0.  A cancel
    issued by the startup reconcile was ticketed at height 0, and its first
    sweep measured an age of nine million heights: a maximum-error raise at
    every boot.  Tickets use the cycle's own height, and none is opened while
    that height is unknown."""
    engine = _engine()
    # last_block_ really is stored in exactly one place, at the end of a cycle:
    # the premise of this test, pinned so a change to it is noticed here.
    assert len(re.findall(r"last_block_\.store\(", engine)) == 1
    cycle = _function_body(engine, "asio::awaitable<void> Engine::on_new_block_coro(")
    stamp = cycle.index("fee_now_block_ = block_height;")
    assert stamp < cycle.index("cycle_.clear();"), "stamped FIRST, before any step can cancel"
    assert stamp < cycle.index("last_block_.store(block_height")
    assert len(re.findall(r"fee_now_block_\s*=", cycle)) == 1

    # Startup: stamped with the startup height BEFORE the reconcile that cancels.
    boot = engine.index("fee_now_block_ = startup_block;")
    assert engine.index("startup_block_ = startup_block;") < boot
    assert boot < engine.index("offer_mgr_->startup_reconcile(")
    # Those are the only two writers.
    assert len(re.findall(r"fee_now_block_\s*=[^=]", engine)) == 2

    track = _function_body(engine, "void Engine::fee_feedback_track_cancel(")
    unknown = track.index("if (fee_now_block_ == 0) {")
    ticket = track.index("fee_tracker_->make_ticket(")
    assert unknown < ticket, "no known height, no ticket"
    assert re.search(r"if \(fee_now_block_ == 0\) \{\s*fee_tickets_\.erase\(offer_id\);\s*return;", track)
    assert "last_block_" not in track
    verdict = _function_body(engine, "void Engine::fee_feedback_on_cancel_verdict(")
    assert "last_block_" not in verdict and "fee_now_block_" in verdict


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


# ---------------------------------------------------------------------------
# [review #163 r8b] EVERY uint64 -> Mojo FEE CONVERSION SATURATES
#
# This PR introduced xop::to_mojo_saturating() and routed FIFTEEN fee
# conversions through it (the helper does not exist on main).  It pinned NONE
# of them, and a final merge gate proved the gap by measurement: on the
# four-way merged tree it dropped the wrapper from each of the four
# CoinLockLedger fee arguments in turn and NOTHING caught it --
#
#   * MSVC /W4 /WX builds clean, because uint64 -> int64 is a SAME-SIZE
#     conversion and -Wconversion is in neither toolchain's flag set;
#   * the C++ suite passes, because nothing in cpp/tests constructs an
#     OfferManager;
#   * no Python scan mentioned to_mojo_saturating at all.
#
# The only thing that had ever held those four lines was #162's literal pin on
# `try_lock_floor_only(0,current_fee_mojos_,min_coin)` -- an ACCIDENT of a
# neighbouring PR.  When #162 rightly loosened it to content matching so it
# would pass both alone and merged, the bare form satisfied it too, and there
# was nothing left.  The invariant is this PR's, so the guard belongs here.
#
# SCOPE, said plainly: the merged code is CORRECT at all four sites, and the
# reachable impact today is negligible -- the narrowing needs a fee above 2^63
# mojos (~9.2 million XCH) and current_fee_mojos_ is clamped by
# fees.max_fee_mojos.  This is GUARD EROSION, not a live defect.
#
# These assertions are POSITIONAL and content-based on purpose.  #162 appends a
# third `min_coin` argument to the two try_lock forms, so a literal match would
# pass alone and fail merged -- which is exactly the failure being repaired.
# ---------------------------------------------------------------------------

DATABASE = REPO / "cpp" / "src" / "database.cpp"
SATURATE = "to_mojo_saturating("

# The fee always arrives SECOND.  #162's `min_coin` lands third, so the index
# holds for both the two- and the three-argument form.
LEDGER_FEE_SINKS = (
    ("try_lock_floor_only", 1, 2),
    ("try_lock", 1, 2),
    ("note_lock", 1, 1),
    ("reserve_bulk_cancel", 1, 1),
)

# Mojo-typed lvalues fed from a std::uint64_t fee.
MOJO_FEE_ASSIGNMENTS = (
    ("offer_manager.cpp", "fill.fee_mojos"),
    ("offer_manager.cpp", "eval.cancel_cost"),
    ("offer_manager.cpp", "const Mojo fee_cap"),
    ("engine.cpp", "f.fee_mojos"),
)

# Every to_mojo_saturating call site, per file.  An exact count is the
# anti-erosion ratchet: dropping one anywhere fails here even at a shape the
# targeted tests above do not model.  Adding a legitimate new fee conversion
# means updating this map ON PURPOSE.
SATURATION_CENSUS = {
    "cpp/src/database.cpp": 1,
    "cpp/src/engine.cpp": 5,
    "cpp/src/execution/offer_manager.cpp": 9,
}

# The shape this PR replaced: a fee expression narrowed by a bare cast.  The
# `-?` covers engine.cpp's negated ledger leg.  `static_cast<std::uint64_t>` is
# a WIDENING of a Mojo and is deliberately not matched.
BARE_FEE_NARROWING = re.compile(
    r"static_cast<\s*(?:Mojo|std::int64_t)\s*>\s*\(\s*-?\s*"
    r"(?:current_fee_mojos_|fee|fee_mojos|[A-Za-z_]\w*\.fee_mojos)\s*[),*]")

# The three take/arbitrage funding sites pay the fee only when the spend asset
# is XCH; the other arm is a literal Mojo, which is why `: Mojo{0}` is the
# stable half of the anchor.  `[^;]*?` cannot cross a statement boundary.
TAKE_FEE_TERNARY = re.compile(
    r'(?:spend_is_xch|spend_asset\s*==\s*"xch")\s*\?(?P<fee>[^;]*?):\s*Mojo\{0\}')


def _split_arguments(args: str) -> list[str]:
    """Top-level comma split of one call's argument text.

    Tracks (), [], {} and string/char literals.  It does NOT track template
    angle brackets, which is safe here only because every call this scan reads
    has its argument count asserted -- a mis-split shows up as a count
    mismatch rather than as a silent pass.
    """
    out: list[str] = []
    depth = 0
    quote = ""
    start = 0
    i = 0
    while i < len(args):
        ch = args[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = ""
        elif ch == '"' or (ch == "'" and not args[i - 1:i].isalnum()):
            quote = ch
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            out.append(args[start:i].strip())
            start = i + 1
        i += 1
    out.append(args[start:].strip())
    return out


def _offer_manager() -> str:
    return _strip_line_comments(_read(OFFER_MANAGER))


def _source(name: str) -> str:
    return {"offer_manager.cpp": _offer_manager(),
            "engine.cpp": _engine(),
            "database.cpp": _strip_line_comments(_read(DATABASE))}[name]


def test_every_coin_lock_ledger_fee_argument_saturates():
    """DERIVED, not enumerated: every call to the four CoinLockLedger entry
    points that take a fee is checked, so a NEW call site is covered the day it
    is written.  These are the four sites the merge gate unwrapped one at a
    time without anything going red, plus note_lock and reserve_bulk_cancel,
    which are the same conversion into the same ledger."""
    text = _offer_manager()
    for callee, fee_index, expected_calls in LEDGER_FEE_SINKS:
        calls = _call_arguments(text, callee)
        assert len(calls) == expected_calls, (
            "expected %d %s call site(s) in offer_manager.cpp, found %d -- this "
            "scan must not pass vacuously" % (expected_calls, callee, len(calls)))
        for raw in calls:
            args = _split_arguments(raw)
            assert len(args) > fee_index, (
                "%s called with %d argument(s); the fee is argument %d: %r"
                % (callee, len(args), fee_index + 1, raw))
            fee_arg = args[fee_index]
            assert "fee" in fee_arg, (
                "argument %d of %s is not the fee any more -- re-derive this "
                "scan rather than deleting it: %r" % (fee_index + 1, callee, raw))
            assert SATURATE in fee_arg, (
                "%s passes a std::uint64_t fee to a Mojo parameter WITHOUT "
                "to_mojo_saturating: %r.  uint64 -> int64 is a same-size "
                "conversion, so neither MSVC /W4 /WX nor GCC -Werror says a "
                "word, and no gtest constructs an OfferManager."
                % (callee, fee_arg))


def test_every_take_fee_ternary_saturates():
    """The three `<spend asset is xch> ? fee : Mojo{0}` arguments that price a
    take's funding check.  A wrapped NEGATIVE fee is dropped by
    add_same_wallet_fee's own `same_wallet_fee <= 0` clause, so the check would
    silently price the spend without the fee the wallet pays."""
    matches = list(TAKE_FEE_TERNARY.finditer(_engine()))
    assert len(matches) == 3, (
        "expected 3 take-fee ternaries in engine.cpp, found %d" % len(matches))
    for m in matches:
        assert SATURATE in m.group("fee"), (
            "take fee ternary narrows without saturating: %r" % m.group(0))


def test_every_mojo_fee_assignment_saturates():
    """Mojo-typed lvalues fed from a std::uint64_t fee.  `fee_cap` is the one
    that also has to saturate BEFORE the conversion: the doubling happens in
    the uint64 domain, so a bare cast would wrap the product first and then
    narrow the wrapped value, which no care at the cast alone would catch."""
    for source_name, lvalue in MOJO_FEE_ASSIGNMENTS:
        text = _source(source_name)
        pattern = re.compile(re.escape(lvalue) + r"\s*=\s*([^;]*);")
        found = pattern.findall(text)
        assert len(found) == 1, (
            "expected exactly one `%s = ...;` in %s, found %d"
            % (lvalue, source_name, len(found)))
        assert SATURATE in found[0], (
            "`%s` in %s takes a uint64 fee without saturating: %r"
            % (lvalue, source_name, found[0].strip()))


def test_the_remaining_two_fee_conversions_saturate():
    """The two that share no shape with anything else: the offer row's fee
    column (SQLite stores a SIGNED 64-bit integer) and the taker fill's ledger
    leg.  post_ledger_fill's own `add("fee", ...)` is deliberately NOT here --
    it negates `fill.fee_mojos`, which is already a Mojo."""
    db = _strip_line_comments(_read(DATABASE))
    binds = [a for a in _call_arguments(db, "bind_int64")
             if _split_arguments(a)[:2] == ["stmt_insert_offer_", "13"]]
    assert len(binds) == 1, (
        "expected one fee bind on the offer row, found %d" % len(binds))
    assert SATURATE in _split_arguments(binds[0])[2], (
        "the offer row's fee column is bound without saturating: %r" % binds[0])

    taker = _function_body(_engine(), "void Engine::record_taker_fill(")
    legs = re.findall(r'add\("fee",\s*AssetId\{"xch"\},\s*([^;]*)\);', taker)
    assert len(legs) == 1, (
        "expected one fee ledger leg in record_taker_fill, found %d" % len(legs))
    assert SATURATE in legs[0], (
        "record_taker_fill's fee leg narrows a uint64 without saturating: %r"
        % legs[0])


def test_no_fee_expression_is_narrowed_by_a_bare_cast():
    """The other direction of the same invariant: the shape this PR replaced
    must not come back.  Ten of the fifteen sites were an explicit
    `static_cast<Mojo>` / `static_cast<std::int64_t>` on a fee; the other five
    had no cast at all, which is why the census below exists as well."""
    for rel in SATURATION_CENSUS:
        text = _strip_line_comments(_read(REPO / rel))
        hits = BARE_FEE_NARROWING.findall(text)
        assert not hits, (
            "%s narrows a fee expression with a bare cast again -- use "
            "xop::to_mojo_saturating: %r" % (rel, hits))


def test_every_uint64_to_mojo_fee_conversion_is_accounted_for():
    """The ratchet.  Fifteen conversions, and the COUNT is what makes dropping
    any one of them visible -- including at a shape the targeted tests above do
    not model."""
    actual = {}
    for rel in SATURATION_CENSUS:
        text = _strip_line_comments(_read(REPO / rel))
        actual[rel] = len(_call_arguments(text, "to_mojo_saturating"))
    assert actual == SATURATION_CENSUS, (
        "to_mojo_saturating call sites moved: %r vs expected %r.  If a fee "
        "conversion was legitimately added or removed, update "
        "SATURATION_CENSUS on purpose -- do not delete this assertion."
        % (actual, SATURATION_CENSUS))
    assert sum(SATURATION_CENSUS.values()) == 15
