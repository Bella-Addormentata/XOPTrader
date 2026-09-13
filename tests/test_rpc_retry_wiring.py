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
