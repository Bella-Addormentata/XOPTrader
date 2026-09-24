"""[WALLET-RESTART-LIVELOCK 2026-09-22] Wallet-restart wiring in the C++
engine, as a source scan.

LINT-CLASS GUARD, disclosed as such.  These tests read cpp/src/engine.cpp and
cpp/include/xop/engine.hpp as TEXT.  They pin call-site WIRING that no C++ unit
test reaches -- Engine is not constructible in xop_tests -- while the DECISION
is pinned by gtest (cpp/tests/test_wallet_sync_watch.cpp).  A pass here says
the engine still asks that decision before restarting the wallet; it says
nothing about runtime behaviour.

What they guard.  Step 8 restarted the Chia wallet service after 20 unsynced
heartbeats, whatever the wallet was doing.  A Chia long sync records its
progress only when it completes, and every restart rolls the wallet back 256
blocks, so on 2026-09-22 nine restarts between 17:18 and 18:24 kept a syncing
wallet from ever finishing and Step 8 managed no offers all evening.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "cpp" / "src" / "engine.cpp"
ENGINE_HPP = REPO / "cpp" / "include" / "xop" / "engine.hpp"

STEP8 = "asio::awaitable<void> Engine::step_manage_offers(BlockHeight block_height)"
RESTART_COMMAND = 'std::system("chia stop wallet'


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


def _engine() -> str:
    return _strip_line_comments(_read(ENGINE))


def _function_body(text: str, signature: str) -> str:
    """The text of the function whose definition starts with `signature`, up to
    the closing brace at column 0."""
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def _matching(text: str, open_index: int) -> int:
    """Index of the bracket closing the one at `open_index`, skipping string
    and character literals.  Comments must already be stripped."""
    pairs = {"(": ")", "{": "}", "[": "]"}
    opener = text[open_index]
    closer = pairs[opener]
    depth = 0
    quote = ""
    i = open_index
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
        elif ch == opener:
            depth += 1
        elif ch == closer:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise AssertionError(f"unbalanced {opener!r} at offset {open_index}")


def _call_args(text: str, callee: str) -> list[list[str]]:
    """The top-level-comma-split arguments of every `<callee>(` in `text`."""
    results: list[list[str]] = []
    for match in re.finditer(re.escape(callee) + r"\s*\(", text):
        open_index = match.end() - 1
        inner = text[open_index + 1:_matching(text, open_index)]
        args, depth, current = [], 0, ""
        for ch in inner:
            if ch in "({[":
                depth += 1
            elif ch in ")}]":
                depth -= 1
            if ch == "," and depth == 0:
                args.append(" ".join(current.split()))
                current = ""
            else:
                current += ch
        args.append(" ".join(current.split()))
        results.append(args)
    return results


def test_no_heartbeat_counter_restarts_the_wallet() -> None:
    for path in (ENGINE, ENGINE_HPP):
        text = _strip_line_comments(_read(path))
        assert "kWalletRestartThreshold" not in text, path.name
        assert "consecutive_unsynced_blocks_" not in text, path.name


def test_the_wallet_restart_is_gated_by_the_sync_watch() -> None:
    """Every wallet-restart command sits inside the block that runs only on a
    Restart verdict -- whatever the platform branch."""
    text = _engine()
    everywhere = text.count(RESTART_COMMAND)
    body = _function_body(text, STEP8)
    assert body.count(RESTART_COMMAND) == everywhere >= 1, (
        "a wallet restart outside Step 8's sync gate"
    )

    gates = [m for m in re.finditer(r"\bif\s*\(", body)
             if "WalletSyncAction::Restart"
             in body[m.end() - 1:_matching(body, m.end() - 1)]]
    assert len(gates) == 1, "expected exactly one Restart-verdict branch"
    cond_close = _matching(body, gates[0].end() - 1)
    block_open = body.index("{", cond_close)
    block = body[block_open:_matching(body, block_open)]
    assert "==" in body[gates[0].end() - 1:cond_close]
    assert block.count(RESTART_COMMAND) == everywhere, (
        "a wallet restart command outside the Restart-verdict branch"
    )


def test_every_sync_reading_reaches_the_watch() -> None:
    """The watch sees EVERY reading -- synced ones too, which is what clears the
    backoff -- as the gate itself defines synced, and an unread `syncing` is
    treated as syncing: never idle, and (review round 2) never synced."""
    body = _function_body(_engine(), STEP8)
    calls = _call_args(body, "execution::observe_wallet_sync")
    assert len(calls) == 1
    args = calls[0]
    assert args[0] == "wallet_sync_watch_"
    assert args[1] == "fully_synced"
    assert args[2] == "may_be_syncing"

    definition = re.search(r"const\s+bool\s+may_be_syncing\s*=\s*([^;]+);", body)
    assert definition, "may_be_syncing is not defined in Step 8"
    assert " ".join(definition.group(1).split()) == (
        'syncing || !sync_status.contains("syncing")'
    )
    synced = re.search(r"const\s+bool\s+fully_synced\s*=\s*([^;]+);", body)
    assert synced and " ".join(synced.group(1).split()) == "synced && !may_be_syncing", (
        "a reply without `syncing` must not read as synced"
    )
    assert re.search(r"\bwallet_synced_\s*=\s*fully_synced\s*;", body), (
        "the flag the rest of the engine reads must agree with the watch"
    )
    assert body.index("const bool fully_synced") < body.index("execution::observe_wallet_sync(")

    # Between the reading and the gate's first return, so a synced reading
    # reaches it as surely as an unsynced one.
    read_at = body.index("wallet_->get_sync_status()")
    observed_at = body.index("execution::observe_wallet_sync(")
    first_return = body.index("co_return", read_at)
    assert read_at < observed_at < first_return


def test_the_restart_line_promises_no_doubling_a_failure_takes_back() -> None:
    """Review round 4: the restart warning said the next attempt's budgets
    double, unconditionally, while a failed command takes the doubling back
    and the failure line says the next attempt keeps the same budget.  The
    warning now says the budgets double if the restart succeeds."""
    body = _function_body(_engine(), STEP8)
    start = body.index('"[Engine] Wallet unsynced for {}s, {}s of it not "')
    end = body.index(");", start)
    text = re.sub(r'"\s*"', "", body[start:end])
    assert "if it succeeds, the next attempt's budgets double" in text, text
    assert "execution::record_failed_wallet_restart(wallet_sync_watch_)" in body[end:], (
        "the failure branch that keeps the budget must follow the warning"
    )


def test_the_unsynced_warning_logs_the_state_the_verdict_used() -> None:
    """Review round 3: a reply without `syncing` is read as syncing
    (may_be_syncing), so the unsynced warning must not print the raw flag's
    false beside the reading it rejected.  It prints the effective state, and
    says when the field was missing."""
    body = _function_body(_engine(), STEP8)
    warn_at = body.index('"[Engine] Step 8: wallet not fully synced "')
    call_at = body.rindex("spdlog::warn(", 0, warn_at)
    args = _call_args(body[call_at:], "spdlog::warn")[0]
    assert args[1:3] == ["synced", "syncing_text"], args
    text = re.search(r"const\s+char\*\s+const\s+syncing_text\s*=\s*([^;]+);", body)
    assert text and text.start() < call_at, "syncing_text is not defined before the warning"
    assert " ".join(text.group(1).split()) == (
        '!sync_status.contains("syncing") ? "missing, read as true" '
        ': (syncing ? "true" : "false")'
    ), text.group(1)


def test_a_failed_restart_command_takes_back_its_backoff() -> None:
    """Review, PR #170: the failure branch of the restart command -- and only
    it -- reports the failure to the watch, so a restart that did not happen
    does not double the next budget."""
    body = _function_body(_engine(), STEP8)
    rc_if = re.search(r"\bif\s*\(\s*rc\s*==\s*0\s*\)\s*\{", body)
    assert rc_if, "the restart command's return code is not checked"
    ok_open = rc_if.end() - 1
    ok_close = _matching(body, ok_open)
    assert body[ok_close + 1:].lstrip().startswith("else"), "no failure branch"
    fail_open = body.index("{", ok_close + 1)
    fail_block = body[fail_open:_matching(body, fail_open)]
    assert "execution::record_failed_wallet_restart(wallet_sync_watch_)" in fail_block
    assert "record_failed_wallet_restart" not in body[ok_open:ok_close]
    assert body.count("record_failed_wallet_restart(") == 1


def test_the_watch_runs_on_a_monotonic_clock() -> None:
    """A wall-clock step (NTP, DST) must not fire or suppress a restart."""
    body = _function_body(_engine(), STEP8)
    now = re.search(r"const\s+std::int64_t\s+now_s\s*=([^;]+);", body)
    assert now, "now_s is not defined in Step 8"
    assert "steady_clock" in now.group(1)
    assert "system_clock" not in now.group(1)
    assert _call_args(body, "execution::observe_wallet_sync")[0][3] == "now_s"
