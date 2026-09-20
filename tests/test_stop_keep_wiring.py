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


def _code(text: str) -> str:
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
    # No co_await outside the cancel block either: nothing else in shutdown()
    # may reach an RPC, and the keep path must not be interleaved with a
    # heartbeat cycle or an operator Cancel All before ioc_.stop().
    assert coro.count("co_await") == cancel_block.count("co_await")


def test_the_keep_branch_is_the_else_of_the_cancel_block_and_only_reports():
    coro = _continuation(_shutdown())
    cancel_block = _block_followed_by(coro, "if(cancels_book){", "structClaimGuard{")
    after = coro[coro.find(cancel_block) + len(cancel_block):]
    assert after.startswith("elseif(keeps_book){report_offers_kept_on_stop();}"), (
        "the keep branch must be the else of the cancel block and call only the "
        "report: found %r" % after[:80])
    _in_order(after, [
        "report_offers_kept_on_stop();",
        "coin_mgr_->unlock_all();",
        "close_connections();",
        "ioc_.stop();",
    ])


def test_the_keep_report_cannot_reach_the_wallet_or_the_intent_file():
    raw = _definition(ENGINE_CPP, "void Engine::report_offers_kept_on_stop()")
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
    report = _code(_definition(ENGINE_CPP, "void Engine::report_offers_kept_on_stop()"))
    assert "if(heartbeat_in_flight_){spdlog::warn(" in report
    # The flag checkpoints really are outside the cycle: none inside it.
    cycle = _code(_definition(
        ENGINE_CPP, "asio::awaitable<void> Engine::on_new_block_coro(BlockHeight block_height)"))
    assert "check_shutdown_flag(" not in cycle and "evaluate_shutdown_flag(" not in cycle


def test_the_engine_header_documents_the_members_the_scans_rely_on():
    header = _code(_text(ENGINE_HPP))
    assert ("std::atomic<util::StopOffersRequest>stop_offers_request_{"
            "util::StopOffersRequest::Unspecified};") in header
    assert "std::atomic<bool>offers_kept_on_stop_{false};" in header
    assert "voidreport_offers_kept_on_stop();" in header
    # shutdown() keeps its signature: a signal handler calls it bare, and every
    # wiring scan finds it by that text.
    assert "voidshutdown();" in header


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
