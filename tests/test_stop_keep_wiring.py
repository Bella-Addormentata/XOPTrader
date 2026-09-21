"""[S74 2026-09-20] A stop that KEEPS the offers reaches no cancel. Source scan.

LINT-CLASS GUARD, disclosed as such (precedent: tests/test_rpc_retry_wiring.py,
tests/test_offer_status_write_sites.py). The decisions are pure and pinned by
gtest -- the policy table in cpp/tests/test_stop_offers_policy.cpp, the
"offers=" line in cpp/tests/test_shutdown_flag.cpp. Nothing in cpp/tests can
construct an Engine (S36), so what Engine::shutdown() DOES with the decision is
pinned here, over the text of cpp/src/engine.cpp. A pass proves the calls are
present, ordered and confined; it says nothing about run-time behaviour.

What would go wrong without each pin is in its assertion message.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE_CPP = REPO / "cpp" / "src" / "engine.cpp"
ENGINE_HPP = REPO / "cpp" / "include" / "xop" / "engine.hpp"
OFFER_MANAGER_CPP = REPO / "cpp" / "src" / "execution" / "offer_manager.cpp"

#: Everything in Engine::shutdown() that submits a cancel, records one, or
#: talks to the wallet on the way to one. A keep stop must reach none of them.
CANCEL_SITES = (
    "cancel_all(",
    "cancel_ids(",
    "watchdog_cancel_book(",
    "write_cancel_intent(",
    "mark_offer_cancel_submitted(",
    "recheck_terminal(",
    "get_sync_status(",
    "cancel_intent_.emplace(",
    "cancel_intent_[",
    "cancel_intent_.erase(",
)


def _text(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def _definition(path: Path, signature: str) -> str:
    """One function definition: from its signature to the first `}` in column 0."""
    text = _text(path)
    start = text.find(signature)
    assert start != -1, f"{signature!r} not found in {path.name}"
    assert text.find(signature, start + 1) == -1, f"{signature!r} defined twice in {path.name}?"
    end = text.find("\n}\n", start)
    assert end != -1, f"end of {signature!r} not found in {path.name}"
    return text[start:end]


def _code(text: str, *, squeeze: bool = True) -> str:
    """Comments removed, string and char literals emptied, ALL whitespace
    squeezed out -- so a scan matches code, not prose or log text. A ' after a
    letter or digit is a digit separator (30'000), not a char literal."""
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
    joined = "".join(out)
    return re.sub(r"\s+", "", joined) if squeeze else joined


def _no_comments(text: str) -> str:
    """Comments removed, string literals KEPT, whitespace kept.

    `_code` empties every literal -- that is its job, and it is exactly why
    this scan could not see the sentence a keep stop actually prints. The two
    guards at the bottom of this file read the source through here instead."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
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
    return "".join(out)


def _brace_safe(src: str) -> str:
    """*src* (comment-free, literals kept) with every literal BODY blanked to
    spaces. Same length, so an offset found here indexes *src* -- and a brace
    inside a format string ("{}") is not counted as a code brace."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        if src[i] == '"':
            j = i + 1
            while j < n and src[j] != '"':
                step = 2 if src[j] == "\\" else 1
                for k in range(j, min(j + step, n)):
                    out[k] = " "
                j += step
            i = j + 1
        else:
            i += 1
    return "".join(out)


def _brace_span(safe: str, start: int) -> tuple[int, int]:
    """(open, close+1) of the balanced block whose `{` is at or after *start*,
    measured over a `_brace_safe` string so literals cannot unbalance it."""
    open_at = safe.find("{", start)
    assert open_at != -1, f"no block after offset {start}"
    depth = 0
    for k in range(open_at, len(safe)):
        if safe[k] == "{":
            depth += 1
        elif safe[k] == "}":
            depth -= 1
            if depth == 0:
                return open_at, k + 1
    raise AssertionError(f"unbalanced block after offset {start}")


def _literals(text: str) -> list[str]:
    """Every double-quoted literal in *text*, comments removed first, with
    ADJACENT literals joined -- a C++ log message is normally written as one
    literal per source line, and the operator reads the concatenation."""
    src = _no_comments(text)
    pieces: list[tuple[int, int, str]] = []
    i, n = 0, len(src)
    while i < n:
        if src[i] == '"':
            j = i + 1
            buf: list[str] = []
            while j < n and src[j] != '"':
                if src[j] == "\\":
                    buf.append(src[j:j + 2])
                    j += 2
                else:
                    buf.append(src[j])
                    j += 1
            pieces.append((i, j + 1, "".join(buf)))
            i = j + 1
        else:
            i += 1
    joined: list[str] = []
    for index, (start, end, body) in enumerate(pieces):
        if index and not src[pieces[index - 1][1]:start].strip():
            joined[-1] += body
        else:
            joined.append(body)
    return joined


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


def _block_at(code: str, start: int) -> str:
    """The balanced `{...}` block whose opening brace is at or after *start*.

    `_block` finds its block by a UNIQUE opener; this one takes a position, for
    the scans that walk several identical gates and must look inside each."""
    open_at = code.find("{", start)
    assert open_at != -1, f"no block after offset {start}"
    depth = 0
    for k in range(open_at, len(code)):
        if code[k] == "{":
            depth += 1
        elif code[k] == "}":
            depth -= 1
            if depth == 0:
                return code[open_at:k + 1]
    raise AssertionError(f"unbalanced block after offset {start}")


def _block_followed_by(code: str, opener: str, following: str) -> str:
    """The balanced block of the ONE `opener` that is immediately followed by
    `following` -- for an opener (`if(cancels_book){`) that occurs more than
    once and is told apart only by what its block begins with."""
    assert opener.endswith("{"), opener
    start = code.find(opener + following)
    assert start != -1, f"{opener + following!r} not found"
    assert code.find(opener + following, start + 1) == -1, (
        f"{opener + following!r} appears more than once")
    depth = 0
    for k in range(start + len(opener) - 1, len(code)):
        if code[k] == "{":
            depth += 1
        elif code[k] == "}":
            depth -= 1
            if depth == 0:
                return code[start:k + 1]
    raise AssertionError(f"unbalanced block after {opener!r}")


def _functions_that_can_reach(path: Path, call: str) -> set[str]:
    """Names of the `Class::function` definitions in *path* from which *call*
    is reachable: those whose body contains it, closed over every function in
    the same file that calls one of them.

    Bodies keep their whitespace (comments and literals are still removed):
    squeezed, `co_await helper(` reads `co_awaithelper(` and the call cannot be
    told from a longer identifier."""
    text = _text(path)
    bodies: dict[str, str] = {}
    starts = [(m.start(), m.group(1)) for m in re.finditer(
        r"^[A-Za-z][^\n;]*?\b[A-Za-z_]+::([A-Za-z_]+)\(", text, re.M)]
    for index, (at, name) in enumerate(starts):
        end = starts[index + 1][0] if index + 1 < len(starts) else len(text)
        bodies[name] = bodies.get(name, "") + _code(text[at:end], squeeze=False)
    reach = {name for name, body in bodies.items() if call in body}
    assert reach, f"{call!r} is called nowhere in {path.name}: the scan is vacuous"
    grew = True
    while grew:
        grew = False
        for name, body in bodies.items():
            if name in reach:
                continue
            # A call, not a definition's own qualified name (`Class::name(`).
            if any(re.search(r"(?<![A-Za-z0-9_:])" + re.escape(other) + r"\s*\(", body)
                   for other in reach):
                reach.add(name)
                grew = True
    return reach


def _function_spans(code: str) -> list[tuple[str, int, int]]:
    """(name, start, end) for each `Class::function` definition in *code*, which
    must already be comment- and literal-stripped WITH its whitespace kept, so
    offsets line up with the rest of the scan."""
    starts = [(m.start(), m.group(1)) for m in re.finditer(
        r"^[A-Za-z][^\n;]*?\b[A-Za-z_]+::([A-Za-z_]+)\(", code, re.M)]
    return [(name, at, starts[i + 1][0] if i + 1 < len(starts) else len(code))
            for i, (at, name) in enumerate(starts)]


def _offer_creating_calls(code: str) -> list[int]:
    """Where an offer is CREATED, as positions in *code*.

    Usually `wallet_->create_offer(` itself. A function that issues that call and
    records nothing (no `state_->upsert_offer(`) is a WRAPPER -- #162 adds exactly
    one, `create_offer_min_coin` -- and then the creating calls are the calls TO
    it, which is where the mark belongs: the window a keep stop must wait out runs
    from the request until the offer is in State, and only the caller knows when
    that is. Written this way so the scan states the property rather than today's
    call shape (the previous round's scan broke on a merged tree for exactly that
    reason, with both PRs' own CI green)."""
    spans = _function_spans(code)
    wrappers = {name for name, start, end in spans
                if "->create_offer(" in code[start:end]
                and "state_->upsert_offer(" not in code[start:end]}
    positions: list[int] = []
    for name, start, end in spans:
        if name in wrappers:
            continue  # its own create IS the wrapper; its callers carry the mark
        body = code[start:end]
        for m in re.finditer(r"->create_offer\(", body):
            positions.append(start + m.start())
        for wrapper in wrappers:
            for m in re.finditer(
                    r"(?<![A-Za-z0-9_:])" + re.escape(wrapper) + r"\s*\(", body):
                positions.append(start + m.start())
    return sorted(positions)


def _in_order(haystack: str, needles: list[str]) -> list[int]:
    found = [haystack.find(s) for s in needles]
    missing = [s for s, at in zip(needles, found) if at == -1]
    assert not missing, f"missing: {missing}"
    assert found == sorted(found), f"out of order: {list(zip(needles, found))}"
    return found


def _shutdown() -> str:
    return _code(_definition(ENGINE_CPP, "void Engine::shutdown()"))


def _continuation(shutdown: str) -> str:
    """The coroutine shutdown() posts: everything from its co_spawn on."""
    at = shutdown.find("asio::co_spawn(ioc_,[this,cancels_book,keeps_book]()")
    assert at != -1, "the shutdown continuation no longer captures the stop plan"
    return shutdown[at:]


# --------------------------------------------------------------------------- #
# The decision is asked once, from the right inputs
# --------------------------------------------------------------------------- #

def test_shutdown_resolves_the_policy_once_through_the_table():
    code = _shutdown()
    assert code.count("util::plan_stop_offers(") == 1
    assert ("util::plan_stop_offers(stop_offers_request_.load(std::memory_order_acquire),"
            "config_.engine.shutdown_offers,dry_run_);") in code, (
        "shutdown() must resolve the request's policy against "
        "engine.shutdown_offers and dry run through util::plan_stop_offers")
    _in_order(code, [
        "if(!stop_requested_.compare_exchange_strong(expected,true)){return;}",
        "util::plan_stop_offers(",
        "constboolcancels_book=stop_plan.action==util::StopBookAction::CancelBook;",
        "constboolkeeps_book=stop_plan.action==util::StopBookAction::KeepBook;",
    ])
    assert code.count("cancels_book=") == 1 and code.count("keeps_book=") == 1, (
        "cancels_book / keeps_book are written a second time: a later override "
        "would let a keep stop cancel")


def test_only_an_honoured_flag_hands_shutdown_a_policy():
    engine = _code(_text(ENGINE_CPP))
    assert engine.count("stop_offers_request_.store(") == 1, (
        "the request's policy is stored somewhere other than the honour path -- "
        "a discarded flag's policy must never colour a later stop")
    evaluate = _code(_definition(
        ENGINE_CPP,
        "util::ShutdownFlagDecision Engine::evaluate_shutdown_flag(util::ShutdownFlagSite site)"))
    _in_order(evaluate, [
        "if(decision.verdict==util::ShutdownFlagVerdict::Discard){",
        "if(!action.request_shutdown){",
        "stop_offers_request_.store(facts.parsed.offers,std::memory_order_release);shutdown();",
    ])


# --------------------------------------------------------------------------- #
# The dead man's switch is disarmed FIRST, and cannot fire during the stop
# --------------------------------------------------------------------------- #

def test_a_keep_stop_disarms_the_watchdog_before_anything_else():
    code = _shutdown()
    keep = _block_followed_by(code, "if(keeps_book){", "offers_kept_on_stop_")
    assert keep == ("if(keeps_book){offers_kept_on_stop_.store(true,std::memory_order_release);"
                    "watchdog_stop_.store(true,std::memory_order_relaxed);}"), (
        "the keep branch must set the latch, then end the watchdog loop, and do "
        "nothing else -- no join (a POSIX signal can run shutdown() ON the "
        "watchdog thread)")
    _in_order(code, [
        "constboolkeeps_book=",
        "if(keeps_book){offers_kept_on_stop_.store(true",
        "spdlog::info(",                       # "Shutdown requested"
        "state_->set_status(BotStatus::ShuttingDown);",
        "last_beat_ms_.store(",
        "poll_timer_.cancel();",
        "asio::co_spawn(",
    ])
    engine = _code(_text(ENGINE_CPP))
    assert engine.count("offers_kept_on_stop_.store(") == 1, (
        "the keep latch is set somewhere other than a keep stop")


def test_the_switch_sends_nothing_once_a_keep_stop_has_begun():
    code = _code(_definition(ENGINE_CPP, "void Engine::watchdog_cancel_book("))
    latch = _block(code, "if(offers_kept_on_stop_.load(std::memory_order_acquire)){")
    assert latch.endswith("return;}"), "the latch must return before any RPC"
    assert "cancel_offers(" not in latch
    _in_order(code, [
        "conststd::lock_guard<std::mutex>cancel_lock(watchdog_cancel_mtx_);",
        "if(offers_kept_on_stop_.load(std::memory_order_acquire)){",
        "cancel_offers(",
    ])
    assert code.count("cancel_offers(") == 1, (
        "a second wallet-wide cancel in the switch is not covered by the latch")


def test_only_a_cancelling_stop_claims_the_graceful_cancel():
    """Operator Cancel All reads graceful_cancel_active_ to decide whether a
    shutdown's sweep is about to take over from it, and the watchdog defers to
    it. A keep stop has no sweep to describe."""
    code = _shutdown()
    before_spawn = code[:code.find("asio::co_spawn(")]
    assert before_spawn.count("graceful_cancel_active_.store(true") == 1
    claim = _block_followed_by(before_spawn, "if(cancels_book){", "graceful_cancel_started_ms_")
    assert "graceful_cancel_active_.store(true,std::memory_order_release);" in claim
    assert "if(!dry_run_){" not in code, (
        "a cancel step is gated on dry run alone again: it would run on a keep stop")


# --------------------------------------------------------------------------- #
# Every cancel site lives inside the one block a keep stop skips
# --------------------------------------------------------------------------- #

def test_every_cancel_site_in_shutdown_is_inside_the_cancel_block():
    code = _shutdown()
    coro = _continuation(code)
    cancel_block = _block_followed_by(coro, "if(cancels_book){", "structClaimGuard{")
    for site in CANCEL_SITES:
        in_function = code.count(site)
        assert in_function == cancel_block.count(site), (
            f"{site} appears {in_function} time(s) in shutdown() but "
            f"{cancel_block.count(site)} inside the cancels_book block -- a cancel "
            "site outside it runs on a KEEP stop too")
    # ...and the scan is not vacuous: the sites it guards are really there.
    for site in ("cancel_all(", "cancel_ids(", "watchdog_cancel_book(",
                 "write_cancel_intent(", "mark_offer_cancel_submitted("):
        assert cancel_block.count(site) >= 1, f"{site} vanished from the cancel path"
    # Outside the cancel block shutdown() awaits exactly ONE thing, and it is a
    # timer: the keep branch's bounded wait for an in-flight post. Nothing else
    # may suspend -- an RPC there could hang a keep stop on a wedged wallet.
    outside = coro.replace(cancel_block, "", 1)
    assert outside.count("co_await") == 1, (
        "shutdown() awaits something new outside the cancel block: %d awaits"
        % outside.count("co_await"))
    assert "co_awaitdrain_timer.async_wait(asio::use_awaitable);" in outside


def test_the_keep_branch_is_the_else_of_the_cancel_block_and_only_waits_and_reports():
    coro = _continuation(_shutdown())
    cancel_block = _block_followed_by(coro, "if(cancels_book){", "structClaimGuard{")
    after = coro[coro.find(cancel_block) + len(cancel_block):]
    assert after.startswith("elseif(keeps_book){"), (
        "the keep branch must be the else of the cancel block: found %r" % after[:60])
    keep = _block(after, "elseif(keeps_book){")
    for forbidden in ("wallet_->", "offer_mgr_->", "full_node_->", "dexie_->",
                      "db_->", "co_spawn") + CANCEL_SITES:
        assert forbidden not in keep, (
            f"the keep branch reaches {forbidden}: a keep stop sends no cancel "
            "and must not depend on the wallet answering")
    assert keep.count("co_await") == 1 and keep.endswith(
        "report_offers_kept_on_stop(waited_for_post_ms,post_abandoned);}"), (
        "the keep branch must end in the report, after at most a timer wait")
    _in_order(after, [
        "report_offers_kept_on_stop(waited_for_post_ms,post_abandoned);",
        "coin_mgr_->unlock_all();",
        "close_connections();",
        "ioc_.stop();",
    ])


def test_a_keep_stop_waits_for_an_in_flight_post_and_only_so_long():
    """[review #165] A signal can deliver a keep stop while Step 8 is awaiting
    create_offer. Stopping the io_context at once let the wallet finish a create
    nobody recorded, and the next boot may CANCEL that orphan. The decision is
    gtest-pinned (KeepStopDrain); this pins that the keep branch asks it, with
    the engine's own flag and the header's budget, waits on a timer only, and
    gives up rather than hang."""
    code = _shutdown()
    coro = _continuation(code)
    keep = _block(coro, "elseif(keeps_book){")
    loop = _block(keep, "for(;;){")
    _in_order(loop, [
        "util::keep_stop_drain_step(posting_in_flight_,waited_for_post_ms,"
        + "keep_stop_drain_budget_ms_);",
        "if(drain==util::KeepStopDrainStep::Proceed)break;",
        "if(drain==util::KeepStopDrainStep::GiveUp){post_abandoned=true;break;}",
        "drain_timer.expires_after(std::chrono::milliseconds(util::kKeepStopDrainPollMs));",
        "co_awaitdrain_timer.async_wait(asio::use_awaitable);",
    ])
    assert keep.count("util::keep_stop_drain_step(") == 1
    assert keep.find("for(;;){") < keep.find("report_offers_kept_on_stop("), (
        "the report runs before the wait: it would count a book still being posted")
    # [review round 3] THE BUDGET IS SIZED, NOT GUESSED, and it is sized in the
    # CONSTRUCTOR: one create's whole retry ladder (the wallet client's own
    # request timeout) plus the publish that stands between its answer and the
    # offer entering State. shutdown() only reads the number -- a POSIX signal
    # can run it on any thread, and the keep branch must touch no client.
    # Nothing of the sort is built on the way there. shutdown() runs on the
    # caller's thread -- a POSIX signal handler, for the one stop that can find
    # a create in flight -- and everything below the co_spawn runs on the
    # io_context instead (the S46 cancel ladder reads the wallet's timeout
    # there, which is why this is scoped to the part above it).
    before_spawn = code[:code.find("asio::co_spawn(")]
    for built_in_shutdown in ("rpc::ChiaRPCConfig", "rpc::DexieConfig",
                              "util::rpc_call_worst_case_ms(",
                              "util::keep_stop_drain_budget_ms(",
                              "wallet_->request_timeout("):
        assert built_in_shutdown not in before_spawn, (
            f"shutdown() builds {built_in_shutdown} on the caller's thread: a "
            "POSIX signal can deliver it, and constructing a config there means "
            "allocating in a signal handler")
    ctor = _code(_definition(ENGINE_CPP, "Engine::Engine(const AppConfig& config, bool dry_run,"))
    _in_order(ctor, [
        "offer_mgr_->set_posting_in_flight_flag(&posting_in_flight_);",
        "keep_stop_drain_budget_ms_=util::keep_stop_drain_budget_ms(",
        "rpc_worst_case(wal_cfg.request_timeout.count(),wal_cfg.max_retries,",
        "rpc_worst_case(dexie_defaults.request_timeout.count(),",
    ])
    assert "util::rpc_call_worst_case_ms(" in ctor, (
        "the budget stopped being derived from the clients' own timeouts")
    engine = _code(_text(ENGINE_CPP))
    assert engine.count("keep_stop_drain_budget_ms_=") == 1, (
        "the drain budget is written twice: a later override could shrink it "
        "below the window it waits on")


def test_every_create_offer_is_marked_one_at_a_time():
    """[review #165, round 3] The wait is only as good as the mark it reads, and
    a mark that spans a whole LADDER is a mark the budget cannot cover: the live
    config posts up to 2 x num_tiers = 12 creates per pair, each bounded by the
    wallet's request timeout, so a 60 s budget expired with a create still in
    flight -- the orphan the drain exists to prevent.

    So OfferManager marks ONE create at a time, from the request until that
    offer is in State (the Dexie submission sits in between, and an offer
    created but not yet in State is the worst case). Everything created earlier
    in the same ladder is already in State, and the keep stop's flush gives each
    of those an offer_log row.

    Stated over positions rather than over today's call shape: every
    offer-creating call has its own mark, declared before it (see
    _offer_creating_calls -- it follows the call through the wrapper #162
    introduces, so this passes on that merged tree too)."""
    raw = _code(_text(OFFER_MANAGER_CPP), squeeze=False)
    creates = _offer_creating_calls(raw)
    marks = [m.start() for m in re.finditer(
        r"PostingMark\s+[A-Za-z_]+\{posting_in_flight_flag_\};", raw)]
    assert creates, "no create_offer call in offer_manager.cpp: the scan is vacuous"
    assert len(marks) == len(creates), (
        f"{len(creates)} create_offer call(s) but {len(marks)} PostingMark(s): a "
        "create the keep-stop drain cannot see is one it will not wait for")
    for index, (mark, create) in enumerate(zip(marks, creates)):
        assert mark < create, f"create #{index} is issued before its mark is set"
        if index + 1 < len(creates):
            assert creates[index] < marks[index + 1] < creates[index + 1], (
                "two create_offer calls share one PostingMark: the drain would "
                "have to outwait a whole ladder, which its budget is not for")
    # The guard itself: armed on construction, cleared by the destructor as well
    # as by release(), so a throw, a `continue`, a `break` and a coroutine frame
    # destroyed by ioc_.stop() all clear it.
    squeezed = _code(_text(OFFER_MANAGER_CPP))
    assert ("explicitPostingMark(bool*flag)noexcept:flag_(flag)"
            "{if(flag_!=nullptr){*flag_=true;}}") in squeezed
    assert "~PostingMark(){release();}" in squeezed
    assert ("voidrelease()noexcept{if(flag_!=nullptr){*flag_=false;flag_=nullptr;}}"
            in squeezed)
    # A mark is released EARLY only where the offer is already in State, or
    # where the create answered with a failure and there is no offer at all.
    # (A merge that moves these call sites must re-derive them -- that is the
    # point of pinning the neighbouring statement, not just the count.)
    for pinned in ("state_->upsert_offer(pending);posting_mark.release();",
                   "state_->upsert_offer(po);fallback_mark.release();",
                   "state_->upsert_offer(pending);}batch_mark.release();",
                   "if(batch_failed){batch_mark.release();"):
        assert pinned in squeezed, (
            f"{pinned!r} is gone: a mark released before its offer is in State "
            "lets a keep stop proceed while that offer exists nowhere")


def test_no_create_begins_once_a_stop_is_latched():
    """[review #165, round 4 -- Copilot 4058780337] THE DRAIN'S BUDGET IS ONLY A
    BOUND IF NO NEW CREATE CAN START.

    The keep stop's wait suspends on a 50 ms poll timer, which hands control
    straight back to the coroutine it is waiting for. Nothing stopped
    post_quotes going on to the next tier and re-arming the mark, so a budget
    sized for ONE create (247 s) was spent on a LADDER -- and could expire with
    a create still outstanding, which is the orphan the drain exists to
    prevent. Worse than the latency: the engine went on creating new offers
    after the operator asked it to stop.

    The fix is a second, NON-CANCELLING predicate. It cannot be the S31 abort
    predicate: that one CANCELS a create that landed late, which is the one
    thing a keep stop must not do.

    Stated over positions, like the PostingMark scan above, so ONE create left
    ungated fails it -- a scan that only proved the gate exists somewhere would
    pass on a tree where the fallback loop lost its copy."""
    raw = _code(_text(OFFER_MANAGER_CPP), squeeze=False)
    creates = _offer_creating_calls(raw)
    gates = [m.start() for m in re.finditer(
        r"if\s*\(\s*stop_creating_predicate_\s*&&\s*stop_creating_predicate_\(\)\s*\)",
        raw)]
    assert creates, "no create_offer call in offer_manager.cpp: the scan is vacuous"
    assert len(gates) == len(creates), (
        f"{len(creates)} create_offer call(s) but {len(gates)} stop-creating "
        "gate(s): a create that can still begin after the stop latch makes the "
        "drain wait for more than the one create its budget is sized for")
    for index, (gate, create) in enumerate(zip(gates, creates)):
        assert gate < create, f"create #{index} is issued before its gate"
        if index + 1 < len(creates):
            assert creates[index] < gates[index + 1] < creates[index + 1], (
                "two create_offer calls share one stop-creating gate: the "
                "second can still begin after the latch")
        # NON-cancelling, and it really stops: no wallet call, nothing adopted,
        # nothing cancelled, and it leaves the loop rather than logging on.
        body = re.sub(r"\s+", "", _block_at(raw, gate))
        for forbidden in ("cancel", "upsert_offer", "co_await", "escalate_"):
            assert forbidden not in body, (
                f"the stop-creating gate reaches {forbidden}: a keep stop must "
                "leave every offer that already exists exactly where it is")
        assert body.endswith(("break;}", "co_return0;}")), (
            "the stop-creating gate logs and carries on: found %r" % body[-24:])


def test_the_engine_wires_the_stop_creating_predicate_to_the_stop_latch():
    """...to stop_requested_, not to the keep latch: a CANCELLING stop must not
    post a fresh book on top of the sweep it is running over the same coins
    either. Wired beside the abort predicate, which it does not replace."""
    ctor = _code(_definition(
        ENGINE_CPP, "Engine::Engine(const AppConfig& config, bool dry_run,"))
    _in_order(ctor, [
        "offer_mgr_->set_abort_predicate([this]{"
        "returnwatchdog_fired_.load(std::memory_order_acquire);});",
        "offer_mgr_->set_stop_creating_predicate([this]{"
        "returnstop_requested_.load(std::memory_order_acquire);});",
        "offer_mgr_->set_posting_in_flight_flag(&posting_in_flight_);",
    ])
    engine = _code(_text(ENGINE_CPP))
    assert engine.count("offer_mgr_->set_stop_creating_predicate(") == 1, (
        "the stop-creating predicate is wired more than once, or not at all")


def test_a_create_that_got_no_answer_is_recorded_and_the_keep_stop_says_so():
    """[review #165, round 4 -- Copilot 4058780326] A TIMED-OUT CREATE IS NOT
    "NO OFFER".

    PostingMark is cleared by its destructor when create_offer throws, and that
    is right: the mark exists only to keep the io_context alive until THIS
    coroutine reaches state_->upsert_offer, and once the create has thrown
    there is nothing left to wait for. Holding it would burn the drain budget
    and still end with an untracked offer.

    What was wrong was the CLAIM. A transport failure does not prove the wallet
    refused the request (rpc::request_possibly_submitted; #162 encodes the same
    principle for this RPC family), so the keep stop must not report clean
    success. The fact is recorded instead -- one flag, no RPC, no change to the
    drain's bound -- and the keep report says it.

    Positional again: every create has its own note, or a create whose outcome
    nobody knows is one nobody reports."""
    raw = _code(_text(OFFER_MANAGER_CPP), squeeze=False)
    creates = _offer_creating_calls(raw)
    notes = [m.start() for m in re.finditer(
        r"note_create_outcome_unknown\(e", raw)]
    assert len(notes) == len(creates), (
        f"{len(creates)} create_offer call(s) but {len(notes)} recorded "
        "outcome(s): a create that fails with no answer would be reported as "
        "no offer at all")
    for index, (create, note) in enumerate(zip(creates, notes)):
        assert create < note, f"create #{index} records its outcome before it runs"
        if index + 1 < len(creates):
            assert notes[index] < creates[index + 1], (
                "a create's outcome is recorded after the NEXT create begins")

    # Each note sits in its create's OWN transport handler, and that handler
    # comes first: a base ChiaRPCError handler written above it makes the
    # transport one unreachable, and every timed-out create is classed as a
    # refusal again -- silently, because the code still compiles.
    catches = [(m.start(), m.group(1)) for m in re.finditer(
        r"catch\s*\(\s*const\s+rpc::(\w+)\s*&", raw)]
    for index, (create, note) in enumerate(zip(creates, notes)):
        after = [(at, name) for at, name in catches if at > create]
        assert after, f"create #{index} has no handler at all"
        first_at, first_name = after[0]
        assert first_name == "ChiaRPCTransportError", (
            f"create #{index}'s first handler is {first_name}, so its "
            "transport handler is unreachable")
        assert first_at < note, (
            f"create #{index} records its outcome outside its transport handler")
        later = [at for at, _ in catches if at > first_at]
        assert not later or note < later[0], (
            f"create #{index}'s note is in a later handler than the transport "
            "one it belongs to")
    # The rule itself: only a failure that MAY have reached the wallet counts,
    # and the generic predicate is the one that decides.
    note_body = _code(_definition(
        OFFER_MANAGER_CPP, "void OfferManager::note_create_outcome_unknown("))
    _in_order(note_body, [
        "if(!rpc::request_possibly_submitted(e.curl_code(),e.http_code())){return;}",
        "if(create_outcome_unknown_flag_!=nullptr){*create_outcome_unknown_flag_=true;}",
    ])
    for forbidden in ("co_await", "create_offer(", "cancel", "upsert_offer"):
        assert forbidden not in note_body, (
            f"recording the uncertainty reaches {forbidden}: it must send "
            "nothing and wait for nothing")

    engine = _code(_text(ENGINE_CPP))
    assert engine.count(
        "offer_mgr_->set_create_outcome_unknown_flag(&create_outcome_unknown_);") == 1
    for bare in ("create_outcome_unknown_=true;", "create_outcome_unknown_=false;"):
        assert bare not in engine, (
            "the engine writes the uncertainty flag itself: only the create "
            "that failed knows how it failed")
    report = _code(_definition(
        ENGINE_CPP,
        "void Engine::report_offers_kept_on_stop(std::uint64_t waited_for_post_ms,"))
    assert "if(create_outcome_unknown_){spdlog::error(" in report, (
        "the keep report no longer says it may be finishing with an offer the "
        "wallet holds and this process never recorded -- which would make the "
        "'what it created is recorded and counted above' line a false "
        "all-clear")


def test_the_engine_wires_the_drain_flag_and_never_writes_it_itself():
    """The engine cannot see inside post_quotes, so OfferManager owns the flag.
    Without this one line the drain would always read "nothing in flight"."""
    engine = _code(_text(ENGINE_CPP))
    assert engine.count(
        "offer_mgr_->set_posting_in_flight_flag(&posting_in_flight_);") == 1
    for bare in ("posting_in_flight_=true;", "posting_in_flight_=false;"):
        assert bare not in engine, (
            "the engine writes the drain flag itself again: a coarse mark around "
            "post_quotes spans a whole ladder, which the budget is not sized for")


def test_the_one_offer_creating_call_is_the_engines_and_step_8_then_stops():
    """post_quotes is the ONLY call through which the engine creates a maker
    offer, and Step 8 manages nothing further under a keep stop -- the next
    pair's iteration would open with cancels."""
    engine = _code(_text(ENGINE_CPP))
    assert engine.count("offer_mgr_->post_quotes(") == 1, (
        "a second offer-creating call site is not covered by posting_in_flight_")
    step8 = _code(_definition(
        ENGINE_CPP, "asio::awaitable<void> Engine::step_manage_offers(BlockHeight block_height)"))
    _in_order(step8, [
        "intposted=co_awaitoffer_mgr_->post_quotes(",
        "db_->insert_offer(orec);",
        "if(offers_kept_on_stop_.load(std::memory_order_acquire)){",
    ])
    stop = _block(step8, "if(offers_kept_on_stop_.load(std::memory_order_acquire)){")
    assert stop.endswith("co_return;}"), "Step 8 must stop, not carry on to the next pair"
    # Every maker create in OfferManager is reached only through post_quotes.
    # Stated as a PROPERTY, not as today's text: the functions that can reach
    # the wallet's create_offer are closed over their callers inside
    # offer_manager.cpp, and the only one of them the engine calls must be
    # post_quotes. (An open PR moves every create_offer call into a private
    # helper; a scan naming the two functions that hold the call today broke on
    # that merged tree, with both PRs' own CI green.)
    creators = _functions_that_can_reach(OFFER_MANAGER_CPP, "->create_offer(")
    assert "post_quotes" in creators, creators
    engine_calls = set(re.findall(r"offer_mgr_->([A-Za-z_]+)\(", engine))
    assert creators & engine_calls == {"post_quotes"}, (
        "the engine reaches an offer-creating OfferManager call that "
        "posting_in_flight_ does not cover: %r" % sorted(creators & engine_calls))


def test_the_keep_report_cannot_reach_the_wallet_or_the_intent_file():
    raw = _definition(ENGINE_CPP, "void Engine::report_offers_kept_on_stop(std::uint64_t waited_for_post_ms,")
    code = _code(raw)
    assert "asio::awaitable" not in raw.split("{", 1)[0], (
        "the keep report became a coroutine: it could now await an RPC")
    for forbidden in ("co_await", "co_spawn", "wallet_->", "offer_mgr_->", "full_node_->",
                      "dexie_->", "update_offer_status(", "reopen_cancelled",
                      "fs::remove", "std::ofstream") + CANCEL_SITES:
        assert forbidden not in code, (
            f"the keep report reaches {forbidden} -- a keep stop must send no "
            "cancel, write no cancel intent and change no offer_log status")
    # What it IS there to do.
    _in_order(code, [
        "if(db_->query_offer_status(po.offer_id).has_value()){continue;}",
        "db_->insert_offer(offer_log_row_for(po));",
        "execution::summarise_kept_book(",
        "execution::describe_kept_book(summary)",
    ])
    assert code.count("insert_offer(") == 1
    # Both forms of the keep line -- the ordinary one and the stop-during-boot
    # one -- are at warn. (_code() empties string literals, so this reads raw.)
    flat = re.sub(r"\s+", " ", raw)
    for line_start in ('spdlog::warn("[Engine] [S74] KEEP stop: {} No cancel was sent',
                       'spdlog::warn("[Engine] [S74] KEEP stop during startup'):
        assert line_start in flat, (
            "the keep line must be at warn: main.cpp flushes the file sink at "
            "warn, and the GUI may hard-kill a stop that overruns its window")


def test_a_keep_stop_that_cut_a_cycle_short_says_so():
    """[review #165] A SIGNAL can deliver shutdown() while a heartbeat cycle is
    suspended in an RPC -- shutdown.flag cannot, it is read between cycles. The
    keep continuation then runs to ioc_.stop() without suspending, the cycle
    never resumes, and an offer it was creating at that instant may exist in
    the wallet unrecorded. The engine does not wait the cycle out (it would go
    on posting and cancelling under a stop meant to be quiet); it SAYS so. This
    pins the mark around the cycle and the line that reads it."""
    poll = _code(_definition(ENGINE_CPP, "asio::awaitable<void> Engine::poll_loop_coro()"))
    _in_order(poll, [
        "structCycleMark{bool*flag;~CycleMark(){*flag=false;}}cycle_mark{&heartbeat_in_flight_};",
        "heartbeat_in_flight_=true;",
        "co_awaiton_new_block_coro(current_block);",
    ])
    assert poll.count("heartbeat_in_flight_=true;") == 1
    assert "heartbeat_in_flight_=false;" not in poll, (
        "a bare reset after the await is skipped by an exception; the RAII mark "
        "is what clears it")
    report = _code(_definition(ENGINE_CPP, "void Engine::report_offers_kept_on_stop(std::uint64_t waited_for_post_ms,"))
    # [review -- MERGE BLOCKER] The line must be BUILT FROM the two facts the
    # report computed, not written beside them. Pinning the arguments is the
    # part this scan can do: pinning the SENTENCE is cpp/tests' job (CutCycleLine
    # in test_stop_offers_policy.cpp), because _code() empties every literal.
    assert ('if(heartbeat_in_flight_){spdlog::warn("",'
            "execution::describe_cut_cycle(post_abandoned,create_outcome_unknown_));}"
            ) in report, (
        "the mid-cycle line no longer takes its wording from "
        "execution::describe_cut_cycle(post_abandoned, create_outcome_unknown_) "
        "-- a sentence written here is one no test reads, which is how an "
        "unconditional 'No offer post was left unrecorded.' shipped beside the "
        "two facts that contradict it")
    # ...and a post the bounded wait gave up on is said LOUDER than that.
    assert "if(post_abandoned){spdlog::error(" in report
    # The flag checkpoints really are outside the cycle: none inside it.
    cycle = _code(_definition(
        ENGINE_CPP, "asio::awaitable<void> Engine::on_new_block_coro(BlockHeight block_height)"))
    assert "check_shutdown_flag(" not in cycle and "evaluate_shutdown_flag(" not in cycle


def test_no_step_starts_new_work_after_a_stop_is_latched_mid_cycle():
    """[review #165, round 3] The gate at the TOP of the cycle only sees a stop
    that arrived between cycles. shutdown() is co_spawned onto the same
    io_context, so a signal-delivered stop runs whenever the cycle suspends --
    and a keep stop then WAITS (bounded) for an in-flight create, handing
    control back to the cycle at every poll. Step 8 has its own checks; without
    a gate after it the rest of the cycle carried on under a stop the operator
    had already asked for, and Step 9c can SEND a crossed-book take_offer.

    Gated on stop_requested_, not on the keep latch: a cancelling stop must not
    start new takes either, and its sweep is running over the same coins."""
    cycle = _code(_definition(
        ENGINE_CPP, "asio::awaitable<void> Engine::on_new_block_coro(BlockHeight block_height)"))
    step8 = cycle.find("co_awaitstep_manage_offers(block_height);")
    arb = cycle.find("co_awaitstep_check_arbitrage(block_height);")
    drift = cycle.find("co_awaitstep_run_drift_corrector(block_height);")
    assert -1 not in (step8, arb, drift), "the cycle no longer dispatches these steps"
    assert drift < step8 < arb, "the cycle's step order changed; re-derive the gates"

    gate = _block_followed_by(
        cycle, "if(stop_requested_.load(std::memory_order_acquire)){", "spdlog::warn(")
    at = cycle.find(gate)
    assert step8 < at < arb, (
        "the mid-cycle stop gate is not between Step 8 and Step 9: Steps 9-13 "
        "(arbitrage TAKES, hedging, PnL, the ingest steps, metrics, alerts) "
        "would start after the operator asked to stop")
    assert gate.endswith("co_return;}"), (
        "the gate logs and carries on: it must end the cycle")

    # Step 9f initiates taker trades and runs BEFORE Step 8, so a keep stop's
    # drain cannot hand control back to it -- but a cancelling stop awaits its
    # whole sweep and this cycle resumes in every gap of it.
    assert ("!watchdog_fired_.load(std::memory_order_acquire)&&"
            "!stop_requested_.load(std::memory_order_acquire)&&"
            'wallet_step_may_run("")){try{co_awaitstep_run_drift_corrector('
            "block_height);}") in cycle, (
        "the drift corrector -- the engine's own taker -- is no longer gated on "
        "the stop latch")


def test_the_engine_header_documents_the_members_the_scans_rely_on():
    header = _code(_text(ENGINE_HPP))
    assert ("std::atomic<util::StopOffersRequest>stop_offers_request_{"
            "util::StopOffersRequest::Unspecified};") in header
    assert "std::atomic<bool>offers_kept_on_stop_{false};" in header
    assert ("voidreport_offers_kept_on_stop(std::uint64_twaited_for_post_ms,"
            "boolpost_abandoned);") in header
    assert "boolposting_in_flight_{false};" in header
    assert "boolcreate_outcome_unknown_{false};" in header
    # shutdown() keeps its signature: a signal handler calls it bare, and every
    # wiring scan finds it by that text.
    assert "voidshutdown();" in header


# --------------------------------------------------------------------------- #
# The claims the scan above structurally cannot read
# --------------------------------------------------------------------------- #
#
# [review -- MERGE BLOCKER] EVERY SCAN IN THIS FILE GOES THROUGH _code(), WHICH
# EMPTIES STRING LITERALS. That is correct for matching code and is precisely
# why a flat "No offer post was left unrecorded." survived two rounds of review
# in `report_offers_kept_on_stop`, logged unconditionally forty lines below
# `post_abandoned` and `create_outcome_unknown_` -- the two facts computed to
# say the opposite, and `post_abandoned` implies that branch, so the
# contradiction was guaranteed rather than incidental.
#
# The real fix is structural: every operator-facing SENTENCE a keep stop prints
# is built in xop/execution/kept_book.hpp, where cpp/tests reads exactly what
# the operator reads (`CutCycleLine`, `WatchdogDisarmLine`, `KeptBook` in
# test_stop_offers_policy.cpp). The two guards below keep it that way.
#
# DISCLOSED LIMIT: the second is a VOCABULARY guard, so it catches a returning
# claim in the wording it returns in, not an arbitrary new one. A general
# "no literal in this function may assert a safety property" needs every log
# message moved out of engine.cpp; filed as TODO S77.

#: Reassurance wording that must never be written into a literal in the keep
#: report: it belongs in kept_book.hpp, where a gtest can read it. Each entry
#: is a claim this function's OWN computed facts can contradict.
CLAIM_WORDING = (
    "was left unrecorded",
    "were left unrecorded",
    "nothing was left",
    "nothing was lost",
    "no offer was lost",
    "all-clear",
    "is disarmed for this stop",
    "was a clean stop",
    "settled by the wallet alone",
    "nothing was left on the book",
)


def _report_raw() -> str:
    return _definition(
        ENGINE_CPP,
        "void Engine::report_offers_kept_on_stop(std::uint64_t waited_for_post_ms,")


def test_the_mid_cycle_line_is_a_format_shell_and_nothing_else():
    """The branch that carried the false reassurance may hold ONE literal, and
    it must be a bare format shell -- so every word the operator reads there
    comes from `execution::describe_cut_cycle`, under gtest.

    This is the general form of the guard: any prose written back into the
    branch fails it, whatever the prose says."""
    src = _no_comments(_report_raw())
    safe = _brace_safe(src)
    at = safe.find("if (heartbeat_in_flight_)")
    assert at != -1, "the mid-cycle branch is gone from the keep report"
    open_at, close_at = _brace_span(safe, at)
    branch = src[open_at:close_at]
    assert _literals(branch) == ["[Engine] [S74] {}"], (
        "the mid-cycle branch writes its own words again: found %r. The "
        "sentence must come from execution::describe_cut_cycle, which "
        "CutCycleLine reads; a literal here is invisible to every scan in this "
        "file, because _code() empties it." % (_literals(branch),))


def test_no_literal_in_the_keep_report_asserts_a_safety_property():
    """Vocabulary backstop over the WHOLE report, for the lines that are not
    pure format shells yet. A claim like these is one the report's own facts
    can falsify, so it belongs beside those facts in kept_book.hpp."""
    found = [(claim, text) for text in _literals(_report_raw())
             for claim in CLAIM_WORDING if claim in text.lower()]
    assert not found, (
        "the keep report asserts a safety property in a string literal, which "
        "no scan in this file can read: %r. Build the sentence in "
        "xop/execution/kept_book.hpp instead, where cpp/tests reads exactly "
        "what the operator reads." % (found,))
    # ...and the guard is not vacuous: it really is looking at the operator's
    # words, not at emptied literals.
    assert any("KEEP stop" in text for text in _literals(_report_raw())), (
        "the literal walker found no message text in the keep report: the "
        "guard above is vacuous")


def test_the_switch_clause_is_conditional_on_the_fact_that_decides_it():
    """[review] engine.hpp: "A cancel the switch had ALREADY started before the
    operator asked is not recalled: it holds the mutex, and it was a real
    firing." Both keep lines used to state the disarm flat. `watchdog_fired_`
    is latched BEFORE any switch-initiated cancel and never cleared, so it is
    the fact the sentence needs; the wording itself is under
    `WatchdogDisarmLine`."""
    report = _code(_report_raw())
    assert report.count(
        "execution::describe_watchdog_disarm(watchdog_fired_.load("
        "std::memory_order_acquire))") == 2, (
        "a keep line states the dead man's switch disarm without reading "
        "watchdog_fired_ -- both the ordinary line and the stop-during-boot "
        "one must, and engine.hpp refuses to state it flatly for a reason")


# --------------------------------------------------------------------------- #
# Startup says what the default is, and warns about keep with no expiry
# --------------------------------------------------------------------------- #

def test_startup_prints_the_default_and_warns_when_keep_has_no_expiry():
    text = _text(ENGINE_CPP)
    start = text.index("[S74 2026-09-20] Operator eyeball line for the stop policy")
    block = text[start:text.index("// -- Database (must be first", start)]
    code = _code(block)
    assert "util::stop_offers_policy_name(config_.engine.shutdown_offers)" in code
    warn = _block(code, "if(config_.engine.shutdown_offers==util::StopOffersPolicy::Keep){")
    _in_order(warn, [
        "if(!pair.enabled)continue;",
        ("execution::effective_offer_expiry_secs(pair.offer_expiry_secs_override,"
         + "config_.strategy.offer_expiry_secs)==0"),
        "spdlog::warn(",
    ])
    assert "engine.shutdown_offers is \\\"keep\\\"" in block
