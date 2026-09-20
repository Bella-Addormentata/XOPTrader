"""Every offer the engine creates must go through the min-input-coin floor.

[MIN-INPUT-COIN 2026-09-19] cpp/include/xop/execution/offer_min_input_coin.hpp
decides the floor and the one-retry fallback, and
cpp/tests/test_offer_min_input_coin.cpp pins those decisions.  Nothing there
can see whether OfferManager still USES them: OfferManager is not constructible
in xop_tests (ChiaWalletRPC is final, there is no gmock), and it has three
places that create an offer -- the per-tier path, the merged batch and the
batch fallback.  A fourth posting path calling wallet_->create_offer directly,
or one of the three reverted to it, would leave every C++ test green while
offers are funded from reward dust again.

This is a LINT-CLASS guard over the real source text (precedent:
tests/test_rpc_retry_wiring.py), and it is disclosed as such: it proves the
calls are present and that no create bypasses them, not that they behave at
run time.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CPP_SRC = REPO / "cpp" / "src"
OFFER_MANAGER_CPP = CPP_SRC / "execution" / "offer_manager.cpp"
CHIA_RPC_CPP = CPP_SRC / "rpc" / "chia_rpc.cpp"

WRAPPER_SIGNATURE = "asio::awaitable<json> OfferManager::create_offer_min_coin("
SUBMIT_SIGNATURE = "asio::awaitable<std::string> OfferManager::submit_to_dexie("

# The three posting paths, by the context string each passes to the wrapper.
POSTING_CONTEXTS = ('"tier"', '"merged batch"', '"batch fallback"')


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def _definition(path: Path, signature: str) -> str:
    """One function definition: from its signature to the first `}` in column 0."""
    text = _read(path)
    start = text.find(signature)
    assert start != -1, f"{signature!r} not found in {path.name}"
    assert text.find(signature, start + 1) == -1, f"{signature!r} defined twice in {path.name}?"
    end = text.find("\n}\n", start)
    assert end != -1, f"end of {signature!r} not found in {path.name}"
    return text[start:end]


def _code(text: str, keep_strings: bool = False) -> str:
    """The text with comments removed, string and char literals emptied (unless
    keep_strings), and ALL whitespace squeezed out, so a scan matches code --
    not prose, not log text, not formatting.  A ' after a letter or digit is a
    digit separator (30'000), not a char literal."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1] if keep_strings else c + c)
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


def _call_arguments(code: str, callee: str) -> list[str]:
    """The argument text of every `<callee>(` in whitespace-free code."""
    results: list[str] = []
    for match in re.finditer(re.escape(callee) + r"\(", code):
        depth = 0
        for k in range(match.end() - 1, len(code)):
            if code[k] == "(":
                depth += 1
            elif code[k] == ")":
                depth -= 1
                if depth == 0:
                    results.append(code[match.end():k])
                    break
    return results


def _top_level_argument_count(args: str) -> int:
    depth = 0
    count = 1 if args else 0
    quote = ""
    for ch in args:
        if quote:
            if ch == quote:
                quote = ""
        elif ch == '"':
            quote = ch
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            count += 1
    return count


def test_the_wallet_create_is_called_from_exactly_one_place():
    whole = _code(_read(OFFER_MANAGER_CPP))
    wrapper = _code(_definition(OFFER_MANAGER_CPP, WRAPPER_SIGNATURE))
    assert whole.count("wallet_->create_offer(") == 1, (
        "offer_manager.cpp calls wallet_->create_offer directly %d times -- every "
        "create must go through create_offer_min_coin, or that path funds its "
        "offers from reward dust" % whole.count("wallet_->create_offer(")
    )
    assert wrapper.count("wallet_->create_offer(") == 1, (
        "the one direct wallet_->create_offer call is no longer inside "
        "create_offer_min_coin"
    )


def test_no_other_source_file_creates_offers():
    offenders = []
    for path in sorted(CPP_SRC.rglob("*.cpp")):
        if path in (OFFER_MANAGER_CPP, CHIA_RPC_CPP):
            continue
        if re.search(r"(->|\.)create_offer\(", _code(_read(path))):
            offenders.append(str(path.relative_to(REPO)))
    assert not offenders, (
        "create_offer is called outside OfferManager, bypassing the "
        "min-input-coin floor: %r" % offenders
    )


def test_the_endpoint_is_posted_only_by_create_offer_with_the_floor_in_its_payload():
    whole = _code(_read(CHIA_RPC_CPP), keep_strings=True)
    assert whole.count('rpc_post("create_offer_for_ids"') == 1, (
        "create_offer_for_ids is posted from more than one place in chia_rpc.cpp"
    )
    body = _code(_definition(CHIA_RPC_CPP, "ChiaWalletRPC::create_offer("), keep_strings=True)
    assert 'rpc_post("create_offer_for_ids",payload)' in body
    assert ("build_create_offer_payload(offer_dict,fee,validate_only,max_time,"
            "min_coin_amount)") in body, (
        "create_offer no longer hands min_coin_amount to the payload builder, so "
        "the floor is computed and then dropped"
    )


def test_the_wrapper_applies_the_rule_and_the_fallback():
    wrapper = _code(_definition(OFFER_MANAGER_CPP, WRAPPER_SIGNATURE))
    assert "constdoublefrac=strategy_cfg_.offer_min_input_coin_frac;" in wrapper, (
        "create_offer_min_coin no longer reads strategy.offer_min_input_coin_frac"
    )
    assert wrapper.count("min_coin=offer_min_input_coin(offer_dict,frac);") == 1, (
        "create_offer_min_coin no longer derives the floor from the offer_dict "
        "it is about to send"
    )
    assert wrapper.count("},min_coin,[") == 1, (
        "the floor handed to the fallback is not the one computed from the offer_dict"
    )
    assert wrapper.count("create_offer_with_min_coin_fallback(") == 1, (
        "create_offer_min_coin no longer goes through the one-retry fallback"
    )
    # The floor the fallback hands to the lambda is what reaches the wallet, as
    # the FIFTH argument (after offer_dict, fee, validate_only, max_time).
    (create_args,) = _call_arguments(wrapper, "wallet_->create_offer")
    assert create_args == "offer_dict,current_fee_mojos_,false,expiry_max_time,coin_floor", (
        "unexpected arguments to the wallet create: %r" % create_args
    )
    assert "[this,&offer_dict,expiry_max_time](std::optional<std::uint64_t>coin_floor)" in wrapper
    # The warning has to name the offer and the constraint.
    warn = _definition(OFFER_MANAGER_CPP, WRAPPER_SIGNATURE)
    assert "[min-input-coin]" in warn
    for token in ("pair.name", "to_string(side)", "tier_index", "min_coin.value_or(0)", "frac"):
        assert token in _code(warn), "the fallback warning no longer logs %s" % token


def test_every_posting_path_creates_through_the_wrapper():
    whole = _code(_read(OFFER_MANAGER_CPP), keep_strings=True)
    calls = _call_arguments(whole, "co_awaitcreate_offer_min_coin")
    assert len(calls) == len(POSTING_CONTEXTS), (
        "expected %d posting paths through create_offer_min_coin, found %d -- a "
        "new path needs a context string here, a removed one needs review"
        % (len(POSTING_CONTEXTS), len(calls))
    )
    for context in POSTING_CONTEXTS:
        # _code squeezes whitespace out of string literals too.
        assert sum(args.endswith("," + context.replace(" ", "")) for args in calls) == 1, (
            "no create_offer_min_coin call is labelled %s" % context
        )
    for args in calls:
        assert ",expiry_max_time,pair," in args, (
            "a posting path dropped its offer expiry on the way through the "
            "wrapper: %r" % args
        )


def test_both_dexie_rejection_shapes_are_checked_for_too_many_inputs():
    body = _definition(OFFER_MANAGER_CPP, SUBMIT_SIGNATURE)
    code = _code(body)
    # HTTP 400 arrives as DexieClientError (the shape seen live); a 200 with
    # success=false arrives as SubmitResult.
    assert code.count("dexie_rejected_too_many_inputs(e.response_body)") == 1
    assert code.count("dexie_rejected_too_many_inputs(result.error_message)") == 1
    assert code.count("note_dexie_too_many_inputs(posting,offer_text.size())") == 2
    client_catch = code.index("catch(constrpc::DexieClientError&e)")
    server_catch = code.index("catch(constrpc::DexieServerError&e)")
    assert client_catch < code.index("dexie_rejected_too_many_inputs(e.response_body)") < server_catch, (
        "the HTTP 400 check is no longer inside the DexieClientError handler"
    )
    # Detection only: this PR must not grow an auto-cancel here.
    assert "cancel" not in code, "submit_to_dexie must not cancel anything"

    note = _definition(OFFER_MANAGER_CPP, "void OfferManager::note_dexie_too_many_inputs(")
    assert "[dexie-too-many-inputs]" in note
    assert "RAISE strategy.offer_min_input_coin_frac" in note, (
        "the warning must tell the operator to RAISE the fraction, not lower it"
    )


def test_every_dexie_submission_names_the_offer():
    whole = _code(_read(OFFER_MANAGER_CPP), keep_strings=True)
    calls = _call_arguments(whole, "co_awaitsubmit_to_dexie")
    assert len(calls) == 3, "expected 3 submit_to_dexie call sites, found %d" % len(calls)
    for args in calls:
        assert _top_level_argument_count(args) == 2 and args.startswith("offer_text,fmt::format("), (
            "a submit_to_dexie call passes no posting label, so a too-many-inputs "
            "warning could not say which offer it was: %r" % args
        )
