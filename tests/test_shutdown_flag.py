"""[shutdown-flag-race 2026-09-12] The GUI half of the addressed stop request.

A closing GUI wrote shutdown.flag for engine PID 15916; a newly launched GUI
terminated that engine before it read the flag; the successor engine honoured
the leftover and stopped. These tests pin the GUI side of the fix: the v1
request format (shared byte-for-byte with cpp/tests/test_shutdown_flag.cpp),
the atomic write, the truthful stop classification and the singleton cleanup.

Pure tests of gui/shutdown_flag.py -- no PySide6.
"""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

import pytest

from gui import shutdown_flag
from gui.shutdown_flag import (
    OffersRequest,
    RequestKind,
    StopOutcome,
    classify_stop_outcome,
    cleanup_after_singleton_kill,
    outcome_leaves_undelivered_flag,
    parse_shutdown_request,
    remove_if_addressed_to,
    render_shutdown_request,
    resolve_shutdown_flag_path,
    write_shutdown_request,
)

REPO = Path(__file__).resolve().parents[1]

# GOLDEN: byte-identical to kGolden in cpp/tests/test_shutdown_flag.cpp.
# test_the_golden_request_is_byte_identical_in_the_engine_test enforces it.
GOLDEN = (
    "xop-shutdown-request v1\npid=15916\nrequested_by_pid=19084\n"
    "written_at=2026-09-12T22:41:07\n"
)
BOM = chr(0xFEFF)
#: Arabic-Indic "42": str.isdigit() is True and int() reads it as 42, but the
#: engine's parser takes ASCII 0-9 only.
ARABIC_INDIC_42 = chr(0x0664) + chr(0x0662)
INCIDENT_WRITE = datetime(2026, 9, 12, 22, 41, 7)


def _flag(tmp_path: Path) -> Path:
    return tmp_path / "data" / shutdown_flag.FLAG_NAME


def _put(path: Path, data: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def _request(pid: int) -> bytes:
    return render_shutdown_request(
        pid, requester_pid=19084, written_at="2026-09-12T22:41:07"
    ).encode("ascii")


# --------------------------------------------------------------------------- #
# The format
# --------------------------------------------------------------------------- #

def test_render_matches_the_golden_request_shared_with_the_engine():
    assert render_shutdown_request(
        15916, requester_pid=19084, written_at="2026-09-12T22:41:07"
    ) == GOLDEN


def test_the_golden_request_is_byte_identical_in_the_engine_test():
    """Two copies of the format live in two files; only this keeps them equal."""
    cpp = (REPO / "cpp" / "tests" / "test_shutdown_flag.cpp").read_text(encoding="utf-8")
    # The C++ source spells each LF as a backslash followed by "n".
    c_literal = '"' + GOLDEN.replace(chr(10), chr(92) + "n") + '"'
    assert c_literal in cpp, (
        "cpp/tests/test_shutdown_flag.cpp no longer carries GOLDEN verbatim -- the "
        "engine's parser and the GUI's writer are being tested against different "
        "requests")


PARSE_CASES = [
    ("golden", GOLDEN, RequestKind.ADDRESSED, 15916),
    ("requester-before-target", "requested_by_pid=19084\npid=15916\n",
     RequestKind.ADDRESSED, 15916),
    ("requester-only", "requested_by_pid=19084\n", RequestKind.UNADDRESSED, None),
    ("pre-fix-gui", "shutdown", RequestKind.UNADDRESSED, None),
    ("empty", "", RequestKind.UNADDRESSED, None),
    ("bom-crlf", BOM + "pid=42\r\n", RequestKind.ADDRESSED, 42),
    ("padded", "pid= 42 \r\n", RequestKind.ADDRESSED, 42),
    ("no-newline", "pid=42", RequestKind.ADDRESSED, 42),
    ("empty-value", "pid=\n", RequestKind.MALFORMED, None),
    ("non-decimal", "pid=abc\n", RequestKind.MALFORMED, None),
    ("zero", "pid=0\n", RequestKind.MALFORMED, None),
    ("trailing-junk", "pid=12x\n", RequestKind.MALFORMED, None),
    ("over-max", "pid=4294967296\n", RequestKind.MALFORMED, None),
    ("max", "pid=4294967295\n", RequestKind.ADDRESSED, 4294967295),
    ("duplicate", "pid=1\npid=2\n", RequestKind.MALFORMED, None),
    # int() accepts all three and the engine accepts none: only the digit rule
    # in _decimal_pid keeps the two parsers agreeing.
    ("plus-sign", "pid=+42\n", RequestKind.MALFORMED, None),
    ("digit-separator", "pid=4_2\n", RequestKind.MALFORMED, None),
    ("non-ascii-digits", "pid=" + ARABIC_INDIC_42 + "\n", RequestKind.MALFORMED, None),
]


@pytest.mark.parametrize(
    ("text", "kind", "pid"),
    [case[1:] for case in PARSE_CASES],
    ids=[case[0] for case in PARSE_CASES],
)
def test_parse_mirrors_the_engine(text, kind, pid):
    parsed = parse_shutdown_request(text)
    assert (parsed.kind, parsed.pid) == (kind, pid)


def test_requester_pid_is_read_for_the_relaunch_wait():
    assert parse_shutdown_request(GOLDEN).requester_pid == 19084

    two = parse_shutdown_request("pid=15916\nrequested_by_pid=1\nrequested_by_pid=2\n")
    assert (two.kind, two.pid, two.requester_pid) == (RequestKind.ADDRESSED, 15916, None)

    junk = parse_shutdown_request("pid=15916\nrequested_by_pid=abc\n")
    assert (junk.kind, junk.pid, junk.requester_pid) == (RequestKind.ADDRESSED, 15916, None)


@pytest.mark.parametrize("bad", [0, shutdown_flag.MAX_PID + 1, -5, True])
def test_render_refuses_a_pid_no_engine_can_have(bad):
    with pytest.raises(ValueError):
        render_shutdown_request(bad, requester_pid=1, written_at="2026-09-12T22:41:07")


# --------------------------------------------------------------------------- #
# Writing it
# --------------------------------------------------------------------------- #

def test_write_goes_through_an_atomic_replace(tmp_path, monkeypatch):
    flag = _flag(tmp_path)
    real_replace = shutdown_flag.os.replace
    calls = []

    def spy(src, dst):
        calls.append((Path(src), Path(dst)))
        return real_replace(src, dst)

    monkeypatch.setattr(shutdown_flag.os, "replace", spy)
    write_shutdown_request(flag, 15916, requester_pid=19084, now=INCIDENT_WRITE)

    assert len(calls) == 1
    src, dst = calls[0]
    assert src.parent == flag.parent
    assert src.name != shutdown_flag.FLAG_NAME
    assert dst == flag
    assert flag.read_bytes() == GOLDEN.encode("ascii")
    assert not list(flag.parent.glob("*.tmp"))


def test_write_retries_a_transient_sharing_violation(tmp_path, monkeypatch):
    flag = _flag(tmp_path)
    real_replace = shutdown_flag.os.replace
    calls = []

    def flaky(src, dst):
        calls.append(dst)
        if len(calls) <= 2:
            raise PermissionError(13, "the engine is reading shutdown.flag")
        return real_replace(src, dst)

    monkeypatch.setattr(shutdown_flag.os, "replace", flaky)
    monkeypatch.setattr(shutdown_flag.time, "sleep", lambda _seconds: None)
    write_shutdown_request(flag, 15916, requester_pid=19084, now=INCIDENT_WRITE)

    assert len(calls) == 3
    assert flag.read_bytes() == GOLDEN.encode("ascii")


def test_remove_if_addressed_to_only_removes_its_own_request(tmp_path):
    flag = _flag(tmp_path)

    _put(flag, _request(15916))
    assert remove_if_addressed_to(flag, 15916) is True
    assert not flag.exists()

    _put(flag, _request(7777))
    assert remove_if_addressed_to(flag, 15916) is False
    assert flag.exists()

    _put(flag, b"shutdown")
    assert remove_if_addressed_to(flag, 15916) is False
    assert flag.exists()

    flag.unlink()
    assert remove_if_addressed_to(flag, 15916) is False


# --------------------------------------------------------------------------- #
# [S74 2026-09-20] The optional "offers=" line: keep or cancel the resting offers
# --------------------------------------------------------------------------- #

# GOLDEN_KEEP / GOLDEN_CANCEL: byte-identical to kGoldenKeep / kGoldenCancel in
# cpp/tests/test_shutdown_flag.cpp (enforced below, like GOLDEN).
GOLDEN_KEEP = GOLDEN + "offers=keep\n"
GOLDEN_CANCEL = GOLDEN + "offers=cancel\n"


def test_render_without_a_policy_is_byte_identical_to_the_pre_policy_request():
    """An upgrade changes nothing: no answer, no line, the same bytes."""
    assert render_shutdown_request(
        15916, requester_pid=19084, written_at="2026-09-12T22:41:07",
        offers_policy=None,
    ) == GOLDEN


@pytest.mark.parametrize(("policy", "golden"), [
    ("keep", GOLDEN_KEEP), ("cancel", GOLDEN_CANCEL)])
def test_render_with_a_policy_appends_exactly_one_offers_line(policy, golden):
    assert render_shutdown_request(
        15916, requester_pid=19084, written_at="2026-09-12T22:41:07",
        offers_policy=policy,
    ) == golden


@pytest.mark.parametrize("golden", [GOLDEN_KEEP, GOLDEN_CANCEL],
                         ids=["keep", "cancel"])
def test_the_policy_goldens_are_byte_identical_in_the_engine_test(golden):
    cpp = (REPO / "cpp" / "tests" / "test_shutdown_flag.cpp").read_text(encoding="utf-8")
    c_literal = '"' + golden.replace(chr(10), chr(92) + "n") + '"'
    assert c_literal in cpp, (
        "cpp/tests/test_shutdown_flag.cpp no longer carries this request "
        "verbatim -- the engine's parser and the GUI's writer are being tested "
        "against different requests")


@pytest.mark.parametrize("bad", ["Keep", "KEEP", " keep", "kep", "", "keep-bids", "yes", True, 1])
def test_render_refuses_a_policy_the_engine_cannot_read(bad):
    """The writer is STRICT where the readers are tolerant: a request must
    never carry a policy the engine would fall back from, because the GUI
    would then believe it had said something."""
    with pytest.raises(ValueError):
        render_shutdown_request(
            15916, requester_pid=19084, written_at="2026-09-12T22:41:07",
            offers_policy=bad)


@pytest.mark.parametrize(("policy", "expected"), [
    (None, OffersRequest.UNSPECIFIED),
    ("keep", OffersRequest.KEEP),
    ("cancel", OffersRequest.CANCEL),
])
def test_write_then_read_round_trips_the_policy(tmp_path, policy, expected):
    flag = _flag(tmp_path)
    write_shutdown_request(flag, 15916, requester_pid=19084, now=INCIDENT_WRITE,
                           offers_policy=policy)
    parsed = shutdown_flag.read_shutdown_request(flag)
    assert parsed is not None
    assert (parsed.kind, parsed.pid, parsed.requester_pid, parsed.offers) == (
        RequestKind.ADDRESSED, 15916, 19084, expected)
    # The address helpers are blind to the policy: a keep request is removed,
    # and recognised as naming its target, exactly like any other.
    assert shutdown_flag.flag_names_pid(flag, 15916)
    assert remove_if_addressed_to(flag, 15916) is True


#: The engine's table, row for row (ShutdownFlagOffersPolicy in
#: cpp/tests/test_shutdown_flag.cpp uses the same contents).
OFFERS_CASES = [
    ("golden-no-line", GOLDEN, RequestKind.ADDRESSED, OffersRequest.UNSPECIFIED),
    ("pre-fix-gui", "shutdown", RequestKind.UNADDRESSED, OffersRequest.UNSPECIFIED),
    ("golden-keep", GOLDEN_KEEP, RequestKind.ADDRESSED, OffersRequest.KEEP),
    ("golden-cancel", GOLDEN_CANCEL, RequestKind.ADDRESSED, OffersRequest.CANCEL),
    ("line-first", "offers=keep\npid=42\n", RequestKind.ADDRESSED, OffersRequest.KEEP),
    ("no-newline", "pid=42\noffers=keep", RequestKind.ADDRESSED, OffersRequest.KEEP),
    ("crlf", "pid=42\r\noffers=keep\r\n", RequestKind.ADDRESSED, OffersRequest.KEEP),
    ("padded-capitalised", "pid=42\noffers= Keep \n", RequestKind.ADDRESSED, OffersRequest.KEEP),
    ("bom", BOM + "offers=KEEP\r\npid=42\r\n", RequestKind.ADDRESSED, OffersRequest.KEEP),
    ("tabs", "pid=42\noffers=\tkeep\t\n", RequestKind.ADDRESSED, OffersRequest.KEEP),
    ("hand-written", "offers=keep\n", RequestKind.UNADDRESSED, OffersRequest.KEEP),
    ("empty-value", "pid=42\noffers=\n", RequestKind.ADDRESSED, OffersRequest.UNRECOGNISED),
    ("prefix", "pid=42\noffers=kee\n", RequestKind.ADDRESSED, OffersRequest.UNRECOGNISED),
    ("later-build", "pid=42\noffers=keep-bids\n", RequestKind.ADDRESSED, OffersRequest.UNRECOGNISED),
    ("yes", "pid=42\noffers=yes\n", RequestKind.ADDRESSED, OffersRequest.UNRECOGNISED),
    ("digit", "pid=42\noffers=1\n", RequestKind.ADDRESSED, OffersRequest.UNRECOGNISED),
    ("commented", "pid=42\n#offers=keep\n", RequestKind.ADDRESSED, OffersRequest.UNSPECIFIED),
    ("two-words", "pid=42\noffers=keep cancel\n", RequestKind.ADDRESSED, OffersRequest.UNRECOGNISED),
    ("contradiction", "pid=42\noffers=keep\noffers=cancel\n", RequestKind.ADDRESSED,
     OffersRequest.UNRECOGNISED),
    ("duplicate", "pid=42\noffers=keep\noffers=keep\n", RequestKind.ADDRESSED,
     OffersRequest.UNRECOGNISED),
    # str.lower() folds U+212A KELVIN SIGN to "k"; the engine compares bytes.
    ("kelvin-sign", "pid=42\noffers=" + chr(0x212A) + "eep\n", RequestKind.ADDRESSED,
     OffersRequest.UNRECOGNISED),
    ("indented", "pid=42\n offers=keep\n", RequestKind.ADDRESSED, OffersRequest.UNSPECIFIED),
    ("other-key", "pid=42\nkeep_offers=keep\n", RequestKind.ADDRESSED, OffersRequest.UNSPECIFIED),
    ("malformed-address", "offers=keep\npid=abc\n", RequestKind.MALFORMED,
     OffersRequest.UNSPECIFIED),
    ("two-pids", "pid=1\noffers=keep\npid=2\n", RequestKind.MALFORMED,
     OffersRequest.UNSPECIFIED),
]


@pytest.mark.parametrize(
    ("text", "kind", "offers"),
    [case[1:] for case in OFFERS_CASES],
    ids=[case[0] for case in OFFERS_CASES],
)
def test_parse_reads_the_offers_line_as_the_engine_does(text, kind, offers):
    parsed = parse_shutdown_request(text)
    assert (parsed.kind, parsed.offers) == (kind, offers)


def test_every_offers_case_is_also_in_the_engine_test():
    """The Python table above and ShutdownFlagOffersPolicy are two copies of one
    rule. Every flag content here that the engine test can spell as a plain C
    string literal must appear there too, so a row cannot be added on one side."""
    cpp = (REPO / "cpp" / "tests" / "test_shutdown_flag.cpp").read_text(encoding="utf-8")
    skipped = {"golden-no-line", "pre-fix-gui", "golden-keep", "golden-cancel", "bom",
               "kelvin-sign"}
    for name, text, _kind, _offers in OFFERS_CASES:
        if name in skipped:
            continue
        c_literal = '"' + (text.replace(chr(13), chr(92) + "r")
                               .replace(chr(10), chr(92) + "n")
                               .replace(chr(9), chr(92) + "t")) + '"'
        assert c_literal in cpp, f"{name}: {c_literal} is not in the engine's test"


# --------------------------------------------------------------------------- #
# How a stop ended
# --------------------------------------------------------------------------- #

# (returncode, request_written, flag_still_names_target, forced)
OUTCOME_CASES = [
    ("graceful", (0, True, False, False), StopOutcome.GRACEFUL),
    ("incident-external-kill", (15, True, True, False), StopOutcome.EXITED_WITHOUT_CONSUMING),
    ("clean-rc-but-unconsumed", (0, True, True, False), StopOutcome.EXITED_WITHOUT_CONSUMING),
    ("flag-gone-rc15", (15, True, False, False), StopOutcome.FLAG_GONE_ABNORMAL_EXIT),
    ("forced-unconsumed", (1, True, True, True), StopOutcome.TERMINATED_BEFORE_CONSUMING),
    ("forced-consumed", (1, True, False, True), StopOutcome.TERMINATED_AFTER_CONSUMING),
    ("write-failed", (1, False, False, True), StopOutcome.TERMINATED_WITHOUT_REQUEST),
    ("unkillable", (None, True, True, True), StopOutcome.STILL_RUNNING),
]


@pytest.mark.parametrize(
    ("facts", "expected"),
    [case[1:] for case in OUTCOME_CASES],
    ids=[case[0] for case in OUTCOME_CASES],
)
def test_classify_stop_outcome(facts, expected):
    returncode, written, still_named, forced = facts
    assert classify_stop_outcome(
        returncode,
        request_written=written,
        flag_still_names_target=still_named,
        forced=forced,
    ) is expected


def test_only_undelivered_outcomes_clean_up():
    undelivered = {outcome for outcome in StopOutcome if outcome_leaves_undelivered_flag(outcome)}
    assert undelivered == {
        StopOutcome.EXITED_WITHOUT_CONSUMING,
        StopOutcome.TERMINATED_BEFORE_CONSUMING,
    }


# --------------------------------------------------------------------------- #
# A relaunched GUI terminated the engine a request was written for
# --------------------------------------------------------------------------- #

def test_singleton_cleanup_removes_a_request_for_an_engine_it_killed(tmp_path):
    flag = _put(_flag(tmp_path), _request(5001))
    message = cleanup_after_singleton_kill(flag, [5001])
    assert message is not None and "5001" in message
    assert not flag.exists()


def test_singleton_cleanup_keeps_every_other_request(tmp_path):
    flag = _flag(tmp_path)

    _put(flag, _request(7777))
    assert cleanup_after_singleton_kill(flag, [5001]) is None
    assert flag.exists(), "a request for an engine this startup did not kill is not ours"

    _put(flag, b"shutdown")
    assert cleanup_after_singleton_kill(flag, []) is None
    assert flag.exists(), "nothing was killed, so an unaddressed request may still be meant"

    _put(flag, b"pid=abc\n")
    assert cleanup_after_singleton_kill(flag, [5001]) is None
    assert flag.exists(), "a malformed request is the engine's to discard"

    _put(flag, b"shutdown")
    assert cleanup_after_singleton_kill(flag, [5001]) is not None
    assert not flag.exists(), "an unaddressed request after a kill was meant for a dead engine"

    assert cleanup_after_singleton_kill(None, [5001]) is None
    assert cleanup_after_singleton_kill(flag, [5001]) is None  # absent file


# --------------------------------------------------------------------------- #
# Where the flag lives, before EngineBridge exists
# --------------------------------------------------------------------------- #

def _write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


@pytest.mark.parametrize("case", [
    "relative-config-path",
    "absolute-config-path",
    "config-wins-over-db-override",
    "db-override-when-config-silent",
    "default",
    "missing-config",
    "secrets-overlay-wins",
    "invalid-config-is-ignored",
])
def test_resolve_shutdown_flag_path(tmp_path, case):
    home = tmp_path / "home"
    config = home / "config.yaml"
    override = tmp_path / "cli" / "state.db"
    db_override = None
    validate = None

    if case == "relative-config-path":
        _write(config, "database:\n  path: store/xop.db\n")
        expected = (home / "store").resolve() / "shutdown.flag"
    elif case == "absolute-config-path":
        elsewhere = (tmp_path / "elsewhere" / "xop.db").resolve()
        _write(config, f"database:\n  path: '{elsewhere.as_posix()}'\n")
        expected = elsewhere.parent / "shutdown.flag"
    elif case == "config-wins-over-db-override":
        _write(config, "database:\n  path: store/xop.db\n")
        db_override = override
        expected = (home / "store").resolve() / "shutdown.flag"
    elif case == "db-override-when-config-silent":
        _write(config, "monitoring:\n  prometheus_port: 9090\n")
        db_override = override
        expected = override.resolve().parent / "shutdown.flag"
    elif case == "default":
        _write(config, "monitoring:\n  prometheus_port: 9090\n")
        expected = (home / "data").resolve() / "shutdown.flag"
    elif case == "missing-config":
        # A first launch: the bootstrap has not copied config.yaml in yet.
        expected = (home / "data").resolve() / "shutdown.flag"
    elif case == "secrets-overlay-wins":
        _write(config, "database:\n  path: store/xop.db\n")
        _write(home / "secrets.yaml", "database:\n  path: vault/xop.db\n")
        expected = (home / "vault").resolve() / "shutdown.flag"
    else:  # invalid-config-is-ignored: EngineBridge applies only a validated config
        _write(config, "database:\n  path: store/xop.db\n")
        db_override = override

        def validate(_config):
            return ["Missing required section 'chia'."]

        expected = override.resolve().parent / "shutdown.flag"

    assert resolve_shutdown_flag_path(config, db_override, validate=validate) == expected
