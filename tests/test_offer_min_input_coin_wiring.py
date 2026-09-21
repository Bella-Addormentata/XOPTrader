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


def _split_arguments(args: str) -> list[str]:
    """The top-level, comma-separated arguments of a whitespace-free call.

    KNOWN LIMIT, stated because this round is about a scan's hidden fragility:
    angle brackets are not tracked (they are ambiguous with comparison
    operators), so an argument that is itself a template with a comma in its
    parameter list -- `std::pair<int,int>{...}` -- would split wrongly and the
    assertion would fail with an argument count, not silently pass.  Failing
    loudly on a shape none of the four guarded calls uses is the safe side of
    that trade; `_top_level_argument_count` below has the same limit.
    """
    out: list[str] = []
    depth = 0
    quote = ""
    cur: list[str] = []
    for ch in args:
        if quote:
            cur.append(ch)
            if ch == quote:
                quote = ""
            continue
        if ch == '"':
            quote = ch
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            out.append("".join(cur))
            cur = []
            continue
        cur.append(ch)
    if cur:
        out.append("".join(cur))
    return out


# An argument that is ZERO however it is spelled or wrapped -- `0`, `Mojo(0)`,
# `to_mojo_saturating(0)`.  Anything naming a variable fails to match, which is
# the property a floor-only admission needs: it charges no principal.
_ZERO_ARGUMENT = re.compile(r"(?:[A-Za-z_][\w:]*\()*0\)*\Z")


def _assert_ledger_admission(body: str, callee: str, principal: str,
                             what: str) -> None:
    """`callee` charges `principal`, the CURRENT FEE, and the min-coin floor.

    MATCHED BY CONTENT, NOT BY SPELLING [review #162, round 6].  These
    assertions used to pin the literal text
    `try_lock_floor_only(0,current_fee_mojos_,min_coin)`.  PR #163 wraps the fee
    at these same four call sites as `to_mojo_saturating(current_fee_mojos_)` --
    a change that conflicts with nothing in THIS file, so git reports no
    conflict and the compiler is happy, and only running this scan against the
    MERGED tree shows the breakage.  A source scan that pins literal text is
    itself a merge hazard: it is the one check a sibling PR can invalidate
    without touching the file it guards.

    What this scan exists to pin is that the admission charges the fee the
    create will pay, against the right principal, WITH the floor the create
    will send -- not how any of those three is spelled.  `principal` is the
    substring the principal argument must CONTAIN, or the literal "0" to
    require that it is zero.
    """
    calls = _call_arguments(body, callee)
    assert len(calls) == 1, (
        "%s: expected exactly one %s( call, found %d" % (what, callee, len(calls))
    )
    args = _split_arguments(calls[0])
    assert len(args) == 3, (
        "%s: %s takes %d arguments, expected 3 (principal, fee, floor): %r"
        % (what, callee, len(args), args)
    )
    if principal == "0":
        assert _ZERO_ARGUMENT.match(args[0]), (
            "%s: a floor-only admission must charge NO principal, but the first "
            "argument is %r" % (what, args[0])
        )
    else:
        assert principal in args[0], (
            "%s: admits against %r, which does not mention %r"
            % (what, args[0], principal)
        )
    assert "current_fee_mojos_" in args[1], (
        "%s: the fee argument is %r, which does not mention current_fee_mojos_ "
        "-- the ledger is not being charged the fee the create will pay"
        % (what, args[1])
    )
    assert args[2] == "min_coin", (
        "%s: the third argument is %r, not min_coin -- the ledger admits "
        "against a different floor from the one the create sends"
        % (what, args[2])
    )


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
    # [review #162, round 6] BY CONTENT, not by spelling -- see
    # _assert_ledger_admission for why a literal argument-list pin is a merge
    # hazard.  The property is that this one endpoint is posted with the
    # payload, and that the payload builder is handed all five values in order
    # -- min_coin_amount above all, or the floor is computed and then dropped.
    (post_args,) = _call_arguments(body, "rpc_post")
    posted = _split_arguments(post_args)
    assert len(posted) == 2 and posted[0] == '"create_offer_for_ids"', (
        "create_offer no longer posts create_offer_for_ids: %r" % posted
    )
    assert "payload" in posted[1], (
        "create_offer posts something other than the built payload: %r" % posted[1]
    )
    (payload_args,) = _call_arguments(body, "build_create_offer_payload")
    built = _split_arguments(payload_args)
    assert len(built) == 5, (
        "the payload builder takes %d arguments, expected 5: %r" % (len(built), built)
    )
    for i, token in enumerate(("offer_dict", "fee", "validate_only", "max_time",
                               "min_coin_amount")):
        assert token in built[i], (
            "argument %d of build_create_offer_payload is %r, which does not "
            "mention %r -- %s" % (
                i + 1, built[i], token,
                "the floor is computed and then dropped"
                if token == "min_coin_amount" else "the payload is built wrong")
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
    # [review #162, round 6] BY CONTENT, not by spelling -- see
    # _assert_ledger_admission.  The property is the five values in order, the
    # fifth being the floor the FALLBACK chose for this attempt (nullopt on the
    # retry), not the literal spelling of any of them.
    (create_args,) = _call_arguments(wrapper, "wallet_->create_offer")
    passed = _split_arguments(create_args)
    assert len(passed) == 5, (
        "the wallet create takes %d arguments, expected 5: %r" % (len(passed), passed)
    )
    for i, (token, why) in enumerate((
            ("offer_dict", "the dict the floor was derived from"),
            ("current_fee_mojos_", "the fee the engine is paying"),
            ("false", "validate_only must stay off -- a validating create builds nothing"),
            ("expiry_max_time", "the requested offer expiry"),
            ("coin_floor", "the floor the fallback chose for THIS attempt"))):
        assert token in passed[i], (
            "argument %d of the wallet create is %r, which does not mention %r "
            "(%s)" % (i + 1, passed[i], token, why)
        )
    assert "[this,&offer_dict,expiry_max_time](std::optional<std::uint64_t>coin_floor)" in wrapper
    # The warning has to name the offer and the constraint.  Read through
    # _code() so a COMMENT cannot satisfy the assertion: the tag has to live in
    # a real string literal (keep_strings), the arguments in real code.
    warn_def = _definition(OFFER_MANAGER_CPP, WRAPPER_SIGNATURE)
    assert "[min-input-coin]" in _code(warn_def, keep_strings=True), (
        "the fallback warning no longer carries the [min-input-coin] tag in a "
        "string literal"
    )
    for token in ("pair.name", "to_string(side)", "tier_index", "min_coin.value_or(0)", "frac"):
        assert token in _code(warn_def), "the fallback warning no longer logs %s" % token


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

    # Read through _code(keep_strings=True) so a COMMENT cannot satisfy any of
    # these: whitespace is squeezed out inside literals too, hence the run-on
    # tokens, and each must therefore live in ONE literal of the warning.
    note = _code(_definition(OFFER_MANAGER_CPP,
                             "void OfferManager::note_dexie_too_many_inputs("),
                 keep_strings=True)
    assert "[dexie-too-many-inputs]" in note
    assert "(chiawalletcoinscombine)" in note, (
        "the warning no longer names the one remedy that removes the cause"
    )
    # [review #162, round 5] THE DIRECTION, CORRECTED.  The input count is not
    # monotone in the fraction: raising it raises the floor, and once the coins
    # at or above the floor cannot cover the amount the wallet refuses and
    # create_offer_with_min_coin_fallback re-sends the create with NO floor, so
    # the offer is unbounded again.  At the shipped 0.01 a floored create's CAT
    # leg cannot exceed 100 inputs -- under the 125 Dexie was measured to
    # accept, with the XCH fee leg one coin on this wallet -- so an offer Dexie
    # refuses for input count was built without a floor, and raising the
    # fraction produces MORE of them, not fewer.
    assert "DoNOTRAISEstrategy.offer_min_input_coin_frac" in note, (
        "the warning must tell the operator NOT to raise the fraction: raising "
        "it makes the wallet refuse the floored create more often, which fires "
        "the no-floor retry that builds the offers Dexie refuses"
    )
    assert "orRAISEstrategy.offer_min_input_coin_frac" not in note, (
        "the withdrawn advice ('or RAISE ... -- lowering it admits MORE dust') "
        "is back in the warning"
    )
    assert "SMALLREDUCTION,neverbelowabout0.008" in note, (
        "the warning no longer bounds the one fraction change that can help: a "
        "small reduction, and not below about 1/124 where the CAT-leg bound "
        "alone reaches the 125 inputs Dexie accepts"
    )
    # [review #162, round 5] The bound is on the CAT leg, not on the offer:
    # the XCH fee coin is a separate selection this CAT-scaled floor does not
    # constrain.  The warning must not promise a whole-offer bound.
    assert "CATLEG" in note and "XCHFEELEG" in note, (
        "the warning no longer distinguishes the CAT leg the floor bounds from "
        "the XCH fee leg it does not"
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


# ---------------------------------------------------------------------------
# [review #162] chia applies the one min_coin_amount to the XCH fee coin too,
# so the XCH lock ledger must admit against the same floor the create sends.
# The selection itself is pinned by gtest (CoinLockLedgerMinCoinTest); these
# pin that BOTH admission helpers hand it over, on BOTH ledger calls, and that
# each posting path admits the very dict it then creates.
# ---------------------------------------------------------------------------

LEDGER_FLOOR = ("constMojomin_coin=ledger_min_coin_mojos("
                "offer_dict,strategy_cfg_.offer_min_input_coin_frac);")


def test_the_cycle_ledger_admits_against_the_floor_the_create_sends():
    body = _code(_definition(OFFER_MANAGER_CPP, "bool OfferManager::xch_ledger_admits("))
    assert body.count(LEDGER_FLOOR) == 1, (
        "xch_ledger_admits no longer derives the floor from the offer_dict it admits"
    )
    # The buy-XCH admission: no principal, the current fee, the floor.  Without
    # the floor the wallet skips coins below it and locks a larger one than the
    # ledger records.
    _assert_ledger_admission(body, "xch_cycle_ledger_.try_lock_floor_only", "0",
                             "the buy-XCH cycle admission")
    _assert_ledger_admission(body, "xch_cycle_ledger_.try_lock", "principal",
                             "the spend-side cycle admission")


def test_the_preflight_probe_admits_against_the_same_floor():
    body = _code(_definition(OFFER_MANAGER_CPP, "bool OfferManager::xch_ledger_probe_admits("))
    assert body.count(LEDGER_FLOOR) == 1
    # Without the floor the probe can keep a side the cycle ledger then refuses.
    _assert_ledger_admission(body, "probe.try_lock_floor_only", "0",
                             "the preflight probe, buy-XCH tiers")
    _assert_ledger_admission(body, "probe.try_lock",
                             "xch_principal_from_offer_dict",
                             "the preflight probe, spend-side tiers")


def test_no_ledger_admission_in_offer_manager_omits_the_floor():
    whole = _code(_read(OFFER_MANAGER_CPP))
    calls = (_call_arguments(whole, ".try_lock") + _call_arguments(whole, ".try_lock_floor_only"))
    assert len(calls) == 4, "expected 4 ledger admissions, found %d: %r" % (len(calls), calls)
    for args in calls:
        assert _top_level_argument_count(args) == 3 and args.endswith(",min_coin"), (
            "a ledger admission passes no min_coin: %r" % args
        )


def test_each_posting_path_admits_the_dict_it_then_creates():
    whole = _code(_read(OFFER_MANAGER_CPP))
    for dict_name in ("offer_dict", "merged_dict", "single_dict"):
        assert whole.count("xch_ledger_admits(%s," % dict_name) == 1, (
            "no ledger admission for %s" % dict_name
        )
        assert whole.count("co_awaitcreate_offer_min_coin(%s," % dict_name) == 1, (
            "%s is admitted by the ledger but created from another dict, so the "
            "floor the ledger models is not the one the wallet receives" % dict_name
        )


# ---------------------------------------------------------------------------
# [review #162, round 2] An uncertain create outcome must not be answered with
# more creates.  The classification is pinned by gtest
# (RpcRetryPolicy.AnUnansweredCreateMayHaveBuiltTheOffer); this pins that
# post_merged_side asks it, and stops before the individual fallback.
# ---------------------------------------------------------------------------

MERGED_SIGNATURE = "asio::awaitable<int> OfferManager::post_merged_side("


def test_an_unanswered_merged_create_is_not_followed_by_individual_creates():
    body = _code(_definition(OFFER_MANAGER_CPP, MERGED_SIGNATURE))
    transport = body.index("catch(constrpc::ChiaRPCTransportError&e)")
    base = body.index("catch(constrpc::ChiaRPCError&e)")
    assert transport < base, (
        "the transport-error handler must come first: ChiaRPCTransportError "
        "derives from ChiaRPCError, so behind it it is unreachable"
    )
    assert body.count(
        "batch_uncertain=rpc::create_possibly_submitted(e.curl_code(),e.http_code());"
    ) == 1, "post_merged_side no longer asks whether the merged create may have landed"
    assert transport < body.index("batch_uncertain=rpc::create_possibly_submitted(") < base

    bypass = body.index("if(batch_failed&&batch_uncertain){")
    fallback = body.index("if(batch_failed){")
    assert bypass < fallback, (
        "the no-answer bypass must run before the individual fallback"
    )
    between = body[bypass:fallback]
    assert "co_return0;" in between and "create_offer_min_coin(" not in between, (
        "after an unanswered merged create the function must return without "
        "creating anything else"
    )


def test_the_create_endpoint_is_never_resend_where_the_fallback_relies_on_it():
    header = _read(REPO / "cpp" / "include" / "xop" / "execution" / "offer_min_input_coin.hpp")
    code = _code(header, keep_strings=True)
    assert ('static_assert(rpc::retry_policy_for_endpoint("create_offer_for_ids")'
            "==rpc::RpcRetryPolicy::NeverResend,") in code, (
        "the fallback's premise is no longer checked at compile time"
    )
    assert code.index("static_assert(rpc::retry_policy_for_endpoint(") < code.index(
        "create_offer_with_min_coin_fallback(Createcreate,"), (
        "the static_assert must sit with the fallback it protects"
    )
