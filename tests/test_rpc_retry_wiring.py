"""rpc_post must consult the per-endpoint re-send policy on BOTH re-send paths.

[BULKCANCEL-B 2026-09-13] cpp/include/xop/rpc/rpc_retry_policy.hpp decides
whether a failed attempt may be sent again, and tests/test_rpc_retry_policy.cpp
pins that decision.  Nothing there can see whether ChiaRPCBase::rpc_post still
ASKS it: rpc_post needs a live mTLS endpoint, perform_request is not virtual
and ChiaWalletRPC is final, so no C++ test can drive a timeout through it.
Reverting either re-send test back to `retryable && attempt < max_tries` would
leave every C++ test green while cancel_offers is re-sent on a timeout again.

This is a LINT-CLASS guard over the real source text (precedent:
tests/test_version_sync.py), and it is disclosed as such: it proves the calls
are present in rpc_post, not that rpc_post behaves correctly at run time.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CHIA_RPC_CPP = REPO / "cpp" / "src" / "rpc" / "chia_rpc.cpp"


def _rpc_post_body() -> str:
    text = CHIA_RPC_CPP.read_text(encoding="utf-8").replace("\r\n", "\n")
    signature = "asio::awaitable<json> ChiaRPCBase::rpc_post("
    start = text.find(signature)
    assert start != -1, "ChiaRPCBase::rpc_post definition not found in chia_rpc.cpp"
    assert text.find(signature, start + 1) == -1, "rpc_post defined twice?"
    # The definition ends at the first closing brace in column 0.
    end = text.find("\n}\n", start)
    assert end != -1, "end of rpc_post not found"
    return text[start:end]


def test_rpc_post_looks_up_the_policy_for_its_endpoint_once():
    body = _rpc_post_body()
    assert body.count("retry_policy_for_endpoint(endpoint)") == 1, (
        "rpc_post must derive its re-send policy from the endpoint it was "
        "called with, exactly once, before the attempt loop"
    )


def test_both_resend_paths_ask_the_policy():
    body = _rpc_post_body()
    assert body.count("may_resend(policy, rc, retryable)") == 1, (
        "the transport-error path no longer consults may_resend -- a timed-out "
        "cancel_offers would be re-sent while the wallet may still be running it"
    )
    assert body.count("may_resend(policy, CURLE_OK, retryable)") == 1, (
        "the HTTP-status path no longer consults may_resend -- a cancel that "
        "reached the handler and got a 5xx would be re-sent"
    )
    # Both `continue` gates must test the policy's answer, not the raw
    # transient flag.
    assert len(re.findall(r"if\s*\(\s*resend\s*&&\s*attempt\s*<\s*max_tries\s*\)", body)) == 2
    assert not re.search(r"if\s*\(\s*retryable\s*&&\s*attempt\s*<\s*max_tries\s*\)", body), (
        "a re-send gate tests `retryable` directly again, bypassing the policy"
    )


# ---------------------------------------------------------------------------
# [review 2026-09-13, round 2] A cancel that failed AFTER it may have reached
# the wallet (rpc::cancel_possibly_submitted) must not be sent again until
# each offer has been re-checked.  The decisions are pure and pinned by gtest
# (cancel_retry.hpp, test_cancel_retry.cpp).  OfferManager and Engine are not
# constructible in xop_tests, so the call sites that act on them are pinned
# here, over the source text, with the same disclosure as above: these prove
# the calls are present and ordered, not that they behave at run time.
#
# [review 2026-09-13, round 3] Added: the shutdown never stamps an offer a
# re-check found resolved; operator Cancel All is bounded by a deadline and
# yields to a shutdown request; an unparseable reply is a transport failure;
# the reload drain waits for an operator Cancel All.  The scans below are also
# hardened against the reintroductions an independent review listed.
# ---------------------------------------------------------------------------

OFFER_MANAGER_CPP = REPO / "cpp" / "src" / "execution" / "offer_manager.cpp"
ENGINE_CPP = REPO / "cpp" / "src" / "engine.cpp"


def _definition(path: Path, signature: str) -> str:
    """One function definition: from its signature to the first `}` in column 0."""
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    start = text.find(signature)
    assert start != -1, f"{signature!r} not found in {path.name}"
    assert text.find(signature, start + 1) == -1, f"{signature!r} defined twice in {path.name}?"
    end = text.find("\n}\n", start)
    assert end != -1, f"end of {signature!r} not found in {path.name}"
    return text[start:end]


def _code(text: str) -> str:
    """The text with comments removed, string and char literals emptied, and
    ALL whitespace squeezed out, so a scan matches code -- not prose, not log
    text, not formatting.  A ' after a letter or digit is a digit separator
    (30'000), not a char literal."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j == -1 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j == -1 else j + 2
        else:
            out.append(c)
            i += 1
    return re.sub(r"\s+", "", "".join(out))


def _block(code: str, opener: str) -> str:
    """The balanced `{...}` block that starts at the ONE occurrence of `opener`."""
    assert opener.endswith("{"), opener
    start = code.find(opener)
    assert start != -1, f"{opener!r} not found"
    assert code.find(opener, start + 1) == -1, f"{opener!r} appears more than once"
    depth = 0
    for k in range(start + len(opener) - 1, len(code)):
        if code[k] == "{":
            depth += 1
        elif code[k] == "}":
            depth -= 1
            if depth == 0:
                return code[start:k + 1]
    raise AssertionError(f"unbalanced block after {opener!r}")


def _in_order(haystack: str, needles: list[str]) -> list[int]:
    found = [haystack.find(s) for s in needles]
    missing = [s for s, at in zip(needles, found) if at == -1]
    assert not missing, f"missing: {missing}"
    assert found == sorted(found), f"out of order: {list(zip(needles, found))}"
    return found


def _operator_branch() -> tuple[str, str]:
    code = _code(_definition(ENGINE_CPP, "void Engine::check_cancel_all_flag()"))
    return code, _block(code, "if(done.bulk_possibly_submitted){")


def test_cancel_all_sends_nothing_more_after_a_possibly_submitted_sweep():
    code = _code(_definition(
        OFFER_MANAGER_CPP,
        "asio::awaitable<OfferManager::CancelOutcome> OfferManager::cancel_all("))

    # The classifier decides, in a transport-error catch placed BEFORE the
    # base-class catch that would otherwise take every failure as a refusal.
    _in_order(code, [
        "catch(constrpc::ChiaRPCTransportError&e){",
        "bulk_possibly_submitted=rpc::cancel_possibly_submitted(e.curl_code(),e.http_code());",
        "catch(constrpc::ChiaRPCError&e){",
    ])

    # A possibly-submitted outcome is decided BEFORE the per-id fallback, in
    # a branch that sends nothing -- and the fallback is the only cancel_ids.
    branches = _in_order(code, [
        "if(bulk_ok){",
        "}elseif(bulk_possibly_submitted){",
        "}elseif(all_offers.empty()){",
    ])
    possibly = _block(code, "elseif(bulk_possibly_submitted){")
    assert "cancel_ids(" not in possibly, (
        "cancel_all re-cancels by id after a sweep that may still be running "
        "-- a second, conflicting spend of every offer the sweep cancelled"
    )
    assert "out.bulk_possibly_submitted=true;" in possibly
    assert code.count("cancel_ids(") == 1
    assert code.find("cancel_ids(") > branches[2]

    # [review 2026-09-13, round 3] No RPC of ANY kind leaves that branch --
    # not cancel_ids, not cancel_offer_charged, not emergency_cancel -- and
    # nothing clears the classifier's answer before the branch reads it: the
    # flag is written by its declaration, the classifier and the outcome only.
    assert "co_await" not in possibly, (
        "the possibly-submitted branch awaits an RPC: every cancel it sends can "
        "queue behind the sweep and build a second, conflicting spend"
    )
    assert code.count("bulk_possibly_submitted=") == 3, (
        "bulk_possibly_submitted is written somewhere else in cancel_all -- a "
        "reset after the catch would send a timed-out sweep to the per-id fallback"
    )


def test_shutdown_ladder_rechecks_before_it_cancels_by_id():
    code = _code(_definition(ENGINE_CPP, "void Engine::shutdown()"))

    assert "res.bulk_possibly_submitted=oc.bulk_possibly_submitted;" in code, (
        "the shutdown driver no longer tells the ladder the sweep got no answer"
    )
    recheck = _block(code, "if(ladder.needs_recheck()){")
    _in_order(recheck, [
        "recheck_terminal(",
        "execution::partition_rechecked_offer(",
        "ladder.record_recheck(std::move(part));",
    ])
    # The ladder loop's only per-id cancel comes after the re-check and is
    # handed what the re-check left outstanding.
    assert code.count("cancel_ids(") == 1
    assert code.find("cancel_ids(ladder.outstanding(),cancel_deadline)") > code.find(
        "if(ladder.needs_recheck()){")


def test_shutdown_tags_rechecked_dead_offers_and_never_stamps_them():
    """[review 2026-09-13, round 3] A shutdown re-check that finds an offer
    CANCELLED or FAILED tags it Submitted and leaves the stamp to the next
    engine's intent sweep, whose writer is wallet-verified; a FILLED one only
    drops its intent.  Stamping from this site added a terminal 'cancelled'
    writer that the cancel-truth branch's offer_log write discipline rejects."""
    code = _code(_definition(ENGINE_CPP, "void Engine::shutdown()"))
    dead = _block(code, "for(constauto&id:ladder.resolved_dead()){")
    assert "cancel_intent_[id]=CancelIntentTag::Submitted;" in dead
    filled = _block(code, "for(constauto&id:ladder.resolved_filled()){")
    assert "cancel_intent_.erase(id);" in filled
    for loop, what in ((dead, "cancelled or failed"), (filled, "filled")):
        for writer in ("update_offer_status(", "mark_offer_cancel_submitted("):
            assert writer not in loop, (
                "the shutdown writes offer_log for a %s offer a re-check found "
                "resolved (%s) -- leave it to the intent sweep" % (what, writer)
            )
    after = code.find("for(constauto&id:ladder.resolved_filled()){")
    assert code.find("write_cancel_intent(cancel_intent_);", after) != -1, (
        "the tags must be written to the intent file after both loops"
    )


def test_operator_cancel_all_waits_and_rechecks_before_it_cancels_by_id():
    code, possibly = _operator_branch()

    # The first pause is one request timeout of this client, and nothing
    # shortens it.
    assert possibly.count(
        "conststd::uint32_twait_ms=execution::wait_after_possibly_submitted_ms("
        "wallet_->request_timeout().count());") == 1, (
        "the wait must be one request timeout of the client that sent the sweep"
    )
    assert possibly.count("wait_ms=") == 1, "wait_ms is written a second time"
    assert "std::uint32_tpause_ms=wait_ms;" in possibly
    _in_order(possibly, [
        "async_wait(asio::use_awaitable);",
        "recheck_terminal(",
        "execution::partition_rechecked_offer(",
        "constboolsweep_running=execution::recheck_shows_sweep_running(part,seen_running);",
        "live=std::move(part.recancel);",
        "pause_ms=execution::possibly_submitted_rewait_ms(sweep_running,",
        "cancel_ids(one_id",
    ])
    # Only what a re-check found live is re-cancelled, one id at a time; an id
    # with no verdict never is.
    assert possibly.count("live=") == 1, (
        "the re-cancel list is filled from something other than the re-check's "
        "live bucket"
    )
    recancel = _block(possibly, "for(constauto&id:live){")
    assert "conststd::vector<std::string>one_id{id};" in recancel
    assert code.count("cancel_ids(") == 1, (
        "operator Cancel All re-cancels by id outside the re-check"
    )
    # The wait and the re-check come before the intent tags and the logs.
    assert code.find("if(done.bulk_possibly_submitted){") < code.find(
        "cancel_intent_[id]=CancelIntentTag::Submitted;")


def test_operator_cancel_all_is_bounded_by_its_deadline():
    """[review 2026-09-13, round 3] The deadline is the wait plus the retry
    budget.  No probe starts past it, the final re-cancel is handed it, and
    the pause-and-re-check loop is planned against it."""
    _, possibly = _operator_branch()
    assert ("conststd::uint64_tdeadline_ms=execution::possibly_submitted_deadline_ms("
            "wait_ms,execution::CancelRetryConfig{});") in possibly
    assert ("constautodeadline=branch_t0+std::chrono::milliseconds("
            "static_cast<std::int64_t>(deadline_ms));") in possibly
    probes = _block(possibly, "for(constauto&id:to_probe){")
    _in_order(probes, ["std::chrono::steady_clock::now()<deadline", "recheck_terminal("])
    assert ("possibly_submitted_rewait_ms(sweep_running,branch_elapsed_ms(),"
            "deadline_ms,recancel_reserve_ms)") in possibly
    recancel = _block(possibly, "for(constauto&id:live){")
    _in_order(recancel, [
        "if(std::chrono::steady_clock::now()>=deadline){",
        "cancel_ids(one_id,deadline)",
    ])


def test_operator_cancel_all_stops_for_a_shutdown():
    """[review 2026-09-13, round 3] shutdown() waits on cancel_all_inflight_,
    so this branch must yield to a shutdown request: while pausing, before
    each probe and before each re-cancel."""
    _, possibly = _operator_branch()
    assert ("constautoshutdown_requested=[this](){returngraceful_cancel_active_.load("
            "std::memory_order_acquire);};") in possibly
    pause = _block(possibly, "while(std::chrono::steady_clock::now()<wake_at){")
    _in_order(pause, [
        "if(shutdown_requested()){stopped_for_shutdown=true;break;}",
        "async_wait(asio::use_awaitable);",
    ])
    probes = _block(possibly, "for(constauto&id:to_probe){")
    _in_order(probes, [
        "if(shutdown_requested())stopped_for_shutdown=true;",
        "if(!stopped_for_shutdown",
        "recheck_terminal(",
    ])
    recancel = _block(possibly, "for(constauto&id:live){")
    _in_order(recancel, [
        "if(stopped_for_shutdown||shutdown_requested()){",
        "cancel_ids(one_id",
    ])
    assert possibly.count("if(stopped_for_shutdown)break;") == 2, (
        "a shutdown seen while pausing or probing must end the loop at once"
    )


def test_operator_cancel_all_clears_its_inflight_flag_on_every_exit():
    code = _code(_definition(ENGINE_CPP, "void Engine::check_cancel_all_flag()"))
    lam = code[code.find("asio::co_spawn("):]
    _in_order(lam, [
        "~InflightGuard(){*flag=false;}",
        "inflight_guard{&cancel_all_inflight_};",
        "try{",
        "autodone=co_awaitoffer_mgr_->cancel_all();",
    ])
    assert "cancel_all_inflight_=false;" not in lam, (
        "a bare clear after the catch is back: an exception that is not a "
        "std::exception, or a frame destroyed during the wait, leaves the flag "
        "set and defers every later cancel"
    )


def test_an_unparseable_cancel_reply_is_classed_as_a_transport_failure():
    """[review 2026-09-13, round 3] A 2xx whose body is not JSON reached the
    handler, which ran the request.  rpc_post must report it as a transport
    failure carrying CURLE_OK and that status, so cancel_all's classifier
    reads it as possibly submitted instead of taking the per-id fallback."""
    parse = _block(_code(_rpc_post_body()), "catch(constjson::parse_error&ex){")
    assert "throwChiaRPCTransportError(" in parse, (
        "an unparseable reply is thrown as a plain ChiaRPCError again -- a "
        "timed-out-looking refusal to cancel_all, which re-cancels per id"
    )
    assert parse.endswith(",http_code,CURLE_OK);}")
    assert "throwChiaRPCError(" not in parse


def test_the_reload_drain_waits_for_an_operator_cancel_all():
    """[review 2026-09-13, round 3] The reload drain for live-disabled pairs
    defers while an operator Cancel All is in flight, reports itself not clean
    so the set is kept, and the retry leg that runs every heartbeat picks it
    up once the flag clears."""
    drain = _code(_definition(
        ENGINE_CPP, "asio::awaitable<bool> Engine::sweep_reload_disabled_offers()"))
    gate = _block(drain, "if(cancel_all_inflight_){")
    assert "co_returnfalse;" in gate, (
        "a deferred drain must report not clean, or the retry leg never runs it"
    )
    assert "reload_pending_cancel_.clear()" not in gate
    assert drain.find("if(cancel_all_inflight_){") < drain.find("selective_cancel("), (
        "the drain cancels per id before it looks at cancel_all_inflight_"
    )
    reload = _code(_definition(
        ENGINE_CPP, "asio::awaitable<void> Engine::check_config_reload_flag()"))
    retry_leg = reload.find(
        "if(!reload_pending_cancel_.empty()){constboolclean=co_awaitsweep_reload_disabled_offers();")
    assert retry_leg != -1, "the heartbeat retry leg for a kept set is gone"
    assert retry_leg < reload.find("if(!fs::exists(config_reload_flag_path_,ec))co_return;"), (
        "the retry leg must run every heartbeat, before the reload flag is looked for"
    )
