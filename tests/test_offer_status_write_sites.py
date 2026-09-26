"""[S14 2026-09-13] offer_log write discipline in the C++ engine, as a source scan.

LINT-CLASS GUARD, disclosed as such.  These tests read cpp/src/engine.cpp and
cpp/src/execution/offer_manager.cpp as TEXT.  They pin call-site WIRING that no
C++ unit test reaches -- Engine and OfferManager are not constructible in
xop_tests -- while the decisions themselves are pinned by gtest
(cpp/tests/test_cancel_escalation.cpp and test_database.cpp).  A pass here says
the engine still CALLS those decisions at every site; it says nothing about
runtime behaviour.

The defect they guard: an accepted cancel RPC used to stamp offer_log
'cancelled' at nine engine sites, three XCH/BYC bids rested takeable for
thirteen days under that label, and the stuck counter, the STOPDRAIN count and
the reload drain each disagreed with the remedy they fed about which offers
were eligible.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "cpp" / "src" / "engine.cpp"
OFFER_MANAGER = REPO / "cpp" / "src" / "execution" / "offer_manager.cpp"

# Terminal 'cancelled' writes that carry a WALLET verdict.
WALLET_VERIFIED_CANCELLED = (
    "s46_startup_db_leg",
    "wallet reported terminal",
    "s46_intent_recovery",
)
# NOT wallet-verified: it stamps on a wallet "No trade" answer and bypasses the
# S25 depth buffer.  Named here so the scan stays exact -- it is an open,
# out-of-scope item of the cancel-truth PR, not something this list endorses.
KNOWN_UNVERIFIED_CANCELLED = ("on_chain_reconcile",)
# [FILL-PROOF 2026-09-23] Verified by the CHAIN, against the wallet: the wallet
# reports these offers CONFIRMED, and the node shows a maker coin unspent while
# another was spent elsewhere, the first spend at confirmation depth
# (execution/fill_proof.hpp).  Not buffered through S25 -- the proof already
# waited out the depth -- and the row it closes is one no fill can ever reach.
CHAIN_VERIFIED_CANCELLED = ("dead_on_chain",)

# Every submit-time writer, by the cause it records.
SUBMIT_REASONS = (
    '"shutdown"',
    '"startup_orphan"',
    '"wallet_pending_cancel_observed"',
    '"utxo_liberation"',
    "block_height, reason",
    '"stuck"',
    '"exposure_floor_rebalance"',
    '"suppressed_capital_free"',
    '"post_quotes_retract"',
    '"xch_recovery"',
    '"ttl_while_stopped"',
    '"s46_intent_recancel"',
    '"operator_cancel_all"',
    '"reload_disabled_pair"',
    '"peg_suspended"',
    '"cancel_escalation_"',
    # [v0.10.24 integration] #160's Step 8 pace pass records its pace reason
    # (pace_idle_ttl, pace_idle_crossed, pace_increasing, pace_tier,
    # pace_above_fv, pace_budget) from pace_why.
    "pace_why[oid]",
    # [S70 2026-09-20] ttl_cancel_mode: expire retires an offer the CHAIN has
    # already made untakeable with a free local cancel.  It is a submit-time
    # write like every other: the wallet's CANCELLED verdict, seen by
    # detect_fills, is still what completes the row, and a CONFIRMED trade is
    # never retired at all (execution::decide_expired_retire), so `filled`
    # keeps winning.
    '"expired_onchain"',
    # [SEED-FAIL-CLOSED review round 2] Step 8 takes down a pair's resting
    # offers while a position it trades is unverified.
    '"unverified_position"',
)
SUBMIT_SITE_COUNT = 21


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def _call_arguments(text: str, callee: str) -> list[str]:
    """The argument text of every `<callee>(` that is code, not a // comment.

    String and character literals are skipped while matching parentheses, so a
    reason such as "price_adverse(" cannot unbalance the scan.  A quote after an
    alphanumeric character is a C++14 digit separator, not a literal.
    """
    results: list[str] = []
    for match in re.finditer(re.escape(callee) + r"\s*\(", text):
        line_start = text.rfind("\n", 0, match.start()) + 1
        if "//" in text[line_start:match.start()]:
            continue
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


def _region(text: str, start_marker: str, end_marker: str) -> str:
    start = text.index(start_marker)
    return text[start:text.index(end_marker, start)]


def _escalation_sweep(engine: str) -> str:
    return _region(engine,
                   "asio::awaitable<void> Engine::escalate_stuck_cancels(BlockHeight block)",
                   "void Engine::check_shutdown_flag()")


def _first_call(text: str, callee: str, start: int = 0) -> int:
    """Offset of the first `<callee>(` at or after `start` that is code, not a //
    comment; -1 when there is none."""
    for match in re.finditer(re.escape(callee) + r"\s*\(", text):
        if match.start() < start:
            continue
        line_start = text.rfind("\n", 0, match.start()) + 1
        if "//" not in text[line_start:match.start()]:
            return match.start()
    return -1


def test_every_cancelled_write_is_wallet_verified():
    calls = [args for args in _call_arguments(_read(ENGINE), "update_offer_status")
             if '"cancelled"' in args]
    allowed = (WALLET_VERIFIED_CANCELLED + KNOWN_UNVERIFIED_CANCELLED
               + CHAIN_VERIFIED_CANCELLED)
    unverified = [args for args in calls if not any(tok in args for tok in allowed)]
    assert not unverified, (
        "a 'cancelled' write without a wallet or chain verdict -- an accepted "
        "cancel is a submission; use mark_offer_cancel_submitted: %r" % unverified
    )
    assert len(calls) == 5, (
        "expected exactly 5 terminal 'cancelled' writers, found %d -- the scan "
        "must not pass vacuously, and a new terminal writer needs review" % len(calls)
    )


def test_every_submit_site_goes_through_mark_offer_cancel_submitted():
    engine = _read(ENGINE)
    calls = _call_arguments(engine, "mark_offer_cancel_submitted")
    assert len(calls) == SUBMIT_SITE_COUNT, (
        "expected %d submit-time writers, found %d" % (SUBMIT_SITE_COUNT, len(calls))
    )
    for reason in SUBMIT_REASONS:
        assert any(reason in args for args in calls), (
            "no mark_offer_cancel_submitted call records %s" % reason
        )
    raw_status_writes = [args for args in _call_arguments(engine, "update_offer_status")
                         if "cancel_pending" in args]
    assert not raw_status_writes, (
        "write cancel_pending through mark_offer_cancel_submitted: %r" % raw_status_writes
    )


def test_forced_cancel_predicate_is_shared_by_every_site():
    engine_calls = _call_arguments(_read(ENGINE), "execution::is_forced_cancel_candidate")
    assert len(engine_calls) == 2, (
        "the Step 8 stuck counter and the STOPDRAIN eligibility count must both "
        "use cancel_stale's predicate; found %d call(s)" % len(engine_calls)
    )
    manager_calls = _call_arguments(_read(OFFER_MANAGER), "is_forced_cancel_candidate")
    assert len(manager_calls) == 1, "cancel_stale must use the shared predicate"


def test_status_mapping_helpers_are_used_at_every_persist_and_restore_site():
    engine = _read(ENGINE)
    assert len(_call_arguments(engine, "pending_offer_from_db")) == 1, (
        "the boot restore must map rows through pending_offer_from_db"
    )
    assert len(_call_arguments(engine, "db_status_for")) == 3, (
        "boot orphan persistence, the reconcile mirror and the post-quote insert "
        "must all persist db_status_for(State)"
    )
    assert not re.search(r'\brec\.status\s*=\s*"pending"', engine), (
        "a literal 'pending' persist would write a live row for an offer whose "
        "cancel is already out"
    )


def test_liberation_and_reload_drain_leave_cancel_pending_to_the_escalation():
    engine = _read(ENGINE)
    liberation = _region(engine, "// Filter to only stale offers.",
                         "if (stale_offers.empty())")
    assert "!po.cancel_pending" in liberation, (
        "UTXO liberation must not zero-fee re-fire a cancel_pending offer"
    )
    reload_drain = _region(engine,
                           "asio::awaitable<bool> Engine::sweep_reload_disabled_offers()",
                           "if (to_cancel.empty() || !offer_mgr_ || dry_run_)")
    assert "!po.cancel_pending" in reload_drain, (
        "the reload drain must not count offers selective_cancel skips"
    )


def test_escalation_runs_beside_the_intent_sweep_above_the_gates():
    engine = _read(ENGINE)
    sweep = engine.index("try { co_await sweep_cancel_intent(block_height); }")
    escalate = engine.index("try { co_await escalate_stuck_cancels(block_height); }")
    gate = engine.index('if (xch_recovery_mode_) {\n        spdlog::info("[Engine] Steps 7-8 SKIPPED')
    assert sweep < escalate < gate, (
        "the escalation must run after the intent sweep and above the Step 7/8 gates"
    )
    assert engine.count("co_await escalate_stuck_cancels(") == 1


def test_the_escalation_never_stamps_terminal_or_uses_the_insecure_fallback():
    engine = _read(ENGINE)
    sweep = _region(engine,
                    "asio::awaitable<void> Engine::escalate_stuck_cancels(BlockHeight block)",
                    "void Engine::check_shutdown_flag()")
    assert not _call_arguments(sweep, "update_offer_status"), (
        "a spent maker coin can be a fill: the escalation must never write a status"
    )
    assert not _call_arguments(sweep, "emergency_cancel")
    manager = _read(OFFER_MANAGER)
    recancel = _region(manager, "OfferManager::recancel_secure(",
                       "bool OfferManager::adopt_wallet_record(")
    assert not _call_arguments(recancel, "emergency_cancel"), (
        "emergency_cancel's last resort is an insecure local cancel"
    )
    assert "/*secure=*/true" in recancel


def test_startup_scan_collects_pending_cancel_records_through_the_bucket_function():
    manager = _read(OFFER_MANAGER)
    assert len(_call_arguments(manager, "startup_scan_bucket")) == 1
    phase_one = _region(manager, "OfferManager::startup_reconcile(",
                        "// ---- Phase 1b: THE DB -> WALLET LEG")
    assert "StartupScanBucket::PendingCancelObserved" in phase_one
    assert "wallet_pending_cancel_.push_back(" in phase_one


def test_every_escalation_gate_yields_to_the_dead_mans_switch():
    """[review, round 2] The dead man's switch latches watchdog_fired_ on its own
    thread, then sends a wallet-wide zero-fee cancel.  The per-candidate gate and
    the re-check before the fee-bearing call read shutdown and Cancel All but not
    the watchdog, so a sweep suspended in an RPC could still pay.  The decision is
    gtest-pinned (CancelEscalationGates); this pins that the sweep reads every
    flag into it and asks it at each gate."""
    sweep = _escalation_sweep(_read(ENGINE))
    snapshot = _region(sweep, "const auto async_gates = [this]() {", "return gates;")
    for flag in ("graceful_cancel_active_.load(", "cancel_all_inflight_",
                 "watchdog_fired_.load("):
        assert flag in snapshot, "the asynchronous gate snapshot must read %s" % flag
    yields = _call_arguments(sweep, "execution::escalation_must_yield")
    assert len(yields) == 3 and all("async_gates()" in args for args in yields), (
        "the entry gate, the per-candidate gate and the pre-fee re-check must each "
        "ask escalation_must_yield(async_gates()); found %r" % yields
    )
    per_candidate = _region(sweep, "for (const auto& candidate : due) {",
                            "// 1. The wallet's word.")
    assert _call_arguments(per_candidate, "execution::escalation_must_yield"), (
        "the per-candidate gate must yield to every asynchronous gate"
    )
    escalate = sweep.index("if (verdict == execution::CancelEscalationVerdict::Escalate)")
    pay = _first_call(sweep, "recancel_secure", escalate)
    assert pay > escalate, "the Escalate branch must pay through recancel_secure"
    assert _call_arguments(sweep[escalate:pay], "execution::escalation_must_yield"), (
        "the last gate before the fee-bearing call must include the dead man's switch"
    )


def test_a_restart_restores_the_escalation_fee_floor():
    """[review, round 2] last_fee_mojos lived only in memory, so after a restart at
    unchanged fees escalation N+1 bid exactly what escalation N had paid -- below
    MEMPOOL_MIN_FEE_INCREASE, refused as a replacement, and still counted.  The
    query is gtest-pinned (EscalationFeeSurvivesARestart); this pins the wiring."""
    sweep = _escalation_sweep(_read(ENGINE))
    first_sighting = _region(sweep, "if (track.anchor_block == 0) {",
                             "if (execution::escalation_probe_due(track, now_block, params)) {")
    seeded = re.search(r"(\w+)\s*=\s*db_->max_cancel_escalation_fee\(offer_id\)",
                       first_sighting)
    assert seeded, "first sighting must read the highest recorded escalation fee"
    assert re.search(r"track\.last_fee_mojos\s*=\s*%s\s*;" % re.escape(seeded.group(1)),
                     first_sighting), (
        "first sighting must seed track.last_fee_mojos from max_cancel_escalation_fee"
    )
    records = _call_arguments(sweep, "mark_offer_cancel_submitted")
    assert records and all(re.search(r",\s*fee\s*$", args) for args in records), (
        "the escalation record must carry the fee it is about to pay: %r" % records
    )


def test_the_escalation_persists_before_it_pays():
    """[review, round 2] The cancel_escalation_N event was written AFTER the
    fee-bearing call, in a try that logged at debug and moved on, so a failed
    write lost a paid attempt: after a restart neither the cap nor the fee floor
    could see it.  Engine is not constructible in xop_tests, so the ordering is
    pinned only here."""
    sweep = _escalation_sweep(_read(ENGINE))
    records = _call_arguments(sweep, "mark_offer_cancel_submitted")
    assert len(records) == 1, (
        "one escalation record, written before the fee -- found %d" % len(records)
    )
    escalate = sweep.index("if (verdict == execution::CancelEscalationVerdict::Escalate)")
    record_at = _first_call(sweep, "mark_offer_cancel_submitted", escalate)
    pay_at = _first_call(sweep, "recancel_secure", escalate)
    assert 0 <= record_at < pay_at, (
        "the escalation must write its record before recancel_secure pays the fee"
    )
    assert re.search(r"\bbreak\s*;", sweep[record_at:pay_at]), (
        "a record that cannot be written must end the sweep before any fee is paid"
    )
