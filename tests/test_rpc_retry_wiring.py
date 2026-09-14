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
# constructible in xop_tests, so the three call sites that act on them are
# pinned here, over the source text, with the same disclosure as above: these
# prove the calls are present and ordered, not that they behave at run time.
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


def test_operator_cancel_all_waits_and_rechecks_before_it_cancels_by_id():
    code = _code(_definition(ENGINE_CPP, "void Engine::check_cancel_all_flag()"))

    possibly = _block(code, "if(done.bulk_possibly_submitted){")
    _in_order(possibly, [
        "wait_after_possibly_submitted_ms(wallet_->request_timeout().count())",
        "expires_after(std::chrono::milliseconds(wait_ms));",
        "async_wait(asio::use_awaitable);",
        "recheck_terminal(",
        "execution::partition_rechecked_offer(",
        "cancel_ids(part.recancel)",
    ])
    assert code.count("cancel_ids(") == 1, (
        "operator Cancel All re-cancels by id outside the re-check"
    )
    # The wait and the re-check come before the intent tags and the logs.
    assert code.find("if(done.bulk_possibly_submitted){") < code.find(
        "cancel_intent_[id]=CancelIntentTag::Submitted;")


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
