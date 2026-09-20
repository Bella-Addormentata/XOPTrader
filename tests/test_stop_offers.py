"""[S74 2026-09-20] Stop the engine: keep the resting offers, or cancel them?

The engine's graceful stop used to cancel the whole book, so offers that had to
survive a restart meant hard-killing the GUI and the engine. A stop now carries
a policy. The GUI asks on Stop Trading and on window close; every stop nobody
can answer (OS session end, a signal) sends NO policy and the engine's own
``engine.shutdown_offers`` decides.

Sections:
  1. the policy spelling -- a mirror of the engine's parser, held to it;
  2. who is asked, and what the answer becomes (no Qt);
  3. what the prompt says about the offers that would be left resting;
  4. the Settings save rule for engine.shutdown_offers (the pure half);
  5. the Qt prompt itself -- built, never exec()'d;
  6. EngineBridge: the policy reaches the stop request, and only an engine
     that can honour "keep" ever receives it.

No real engine is spawned anywhere: the bridge tests drive the real stop logic
with a scripted Popen double, as tests/test_engine_stop_outcome.py does.
"""

from __future__ import annotations

import logging
import os
import sqlite3
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402

from gui import shutdown_flag, stop_offers  # noqa: E402
from gui.stop_offers import (  # noqa: E402
    POLICY_CANCEL,
    POLICY_KEEP,
    RestingSummary,
    StopChoice,
    StopDecision,
    build_prompt_text,
    configured_default,
    decide_stop,
    default_choice,
    effective_expiry_secs,
    merge_shutdown_offers,
    parse_policy,
    policy_to_send,
    read_resting_rows,
    summarise_resting_offers,
)

ENGINE_PID = 15916


# --------------------------------------------------------------------------- #
# 1. The policy spelling
# --------------------------------------------------------------------------- #

#: The engine's StopOffersPolicyParse tests use the same strings
#: (cpp/tests/test_stop_offers_policy.cpp).
READABLE = [
    ("cancel", POLICY_CANCEL), ("keep", POLICY_KEEP), ("Keep", POLICY_KEEP),
    ("KEEP", POLICY_KEEP), (" keep", POLICY_KEEP), ("keep ", POLICY_KEEP),
    ("\tkeep\t", POLICY_KEEP), (" kEeP \t", POLICY_KEEP), ("Cancel", POLICY_CANCEL),
    ("CANCEL", POLICY_CANCEL), ("  cancel  ", POLICY_CANCEL),
]
UNREADABLE = [
    "", " ", "kee", "keeps", "keep-bids", "keep;cancel", "cancel,keep", "cancelled",
    "cancel all", "yes", "no", "true", "false", "1", "0", "k e e p", "keep\n",
    '"keep"', "offers=keep",
]


@pytest.mark.parametrize(("text", "policy"), READABLE)
def test_parse_policy_reads_both_words_with_padding_and_ascii_case(text, policy):
    assert parse_policy(text) == policy


@pytest.mark.parametrize("text", UNREADABLE)
def test_parse_policy_never_guesses(text):
    assert parse_policy(text) is None


@pytest.mark.parametrize("value", [None, True, False, 1, 0, 1.0, ["keep"], {"keep": 1}, b"keep"])
def test_parse_policy_refuses_anything_that_is_not_text(value):
    """YAML turns a bare ``yes``/``on`` into a bool; none of that is a policy."""
    assert parse_policy(value) is None


def test_parse_policy_is_not_fooled_by_unicode_case_folding():
    """U+212A KELVIN SIGN lowers to ``k``. The engine compares bytes."""
    assert chr(0x212A).lower() == "k", "the premise of this test"
    assert parse_policy(chr(0x212A) + "eep") is None
    # Python's str.strip() with no argument also strips NBSP and friends; the
    # engine drops spaces and tabs only.
    assert parse_policy(chr(0xA0) + "keep") is None
    assert parse_policy("keep" + chr(0x2003)) is None


def test_the_python_and_engine_parsers_are_tested_against_the_same_strings():
    cpp = (_REPO / "cpp" / "tests" / "test_stop_offers_policy.cpp").read_text(encoding="utf-8")
    for text in [t for t, _ in READABLE] + UNREADABLE:
        literal = '"' + (text.replace(chr(92), chr(92) * 2)
                             .replace('"', chr(92) + '"')
                             .replace(chr(10), chr(92) + "n")
                             .replace(chr(9), chr(92) + "t")) + '"'
        assert literal in cpp, f"{literal} is not in the engine's parser test"


def test_the_help_token_is_the_engines_byte_for_byte():
    """The GUI offers Keep only to an engine whose --help prints this token, so
    the two copies must not drift -- and main.cpp must actually print it."""
    header = (_REPO / "cpp" / "include" / "xop" / "util"
              / "stop_offers_policy.hpp").read_text(encoding="utf-8")
    assert ('kStopPolicyHelpToken =\n    "' + stop_offers.ENGINE_HELP_TOKEN + '";'
            ) in header.replace("\r\n", "\n")
    main_cpp = (_REPO / "cpp" / "src" / "main.cpp").read_text(encoding="utf-8")
    help_branch = main_cpp[main_cpp.index('if (vm.count("help")) {'):]
    help_branch = help_branch[:help_branch.index("return std::nullopt;")]
    assert "xop::util::kStopPolicyHelpToken" in help_branch, (
        "xop_trader --help no longer prints the stop-policy token: the GUI "
        "would stop offering Keep for an engine that supports it")


def test_engine_help_probe():
    assert stop_offers.engine_help_advertises_stop_policy(
        "Usage...\nStop requests: shutdown.flag offers=cancel|keep (no offers line: ...)\n")
    assert not stop_offers.engine_help_advertises_stop_policy("Usage...\n  --secrets arg\n")
    assert not stop_offers.engine_help_advertises_stop_policy(None)


@pytest.mark.parametrize(("config", "expected"), [
    (None, POLICY_CANCEL),
    ({}, POLICY_CANCEL),
    ({"engine": None}, POLICY_CANCEL),
    ({"engine": "keep"}, POLICY_CANCEL),             # not a mapping: the engine rejects it
    ({"engine": {}}, POLICY_CANCEL),
    ({"engine": {"shutdown_offers": "cancel"}}, POLICY_CANCEL),
    ({"engine": {"shutdown_offers": "keep"}}, POLICY_KEEP),
    ({"engine": {"shutdown_offers": " Keep "}}, POLICY_KEEP),
    ({"engine": {"shutdown_offers": "kep"}}, POLICY_CANCEL),
    ({"engine": {"shutdown_offers": True}}, POLICY_CANCEL),
])
def test_configured_default(config, expected):
    assert configured_default(config) == expected


# --------------------------------------------------------------------------- #
# 2. Who is asked, and what the answer becomes
# --------------------------------------------------------------------------- #

def _answers(choice, calls):
    def ask():
        calls.append(choice)
        return choice
    return ask


# [mutation check 2026-09-20] These two used an ask() that RAISED AssertionError
# "a prompt was shown". decide_stop deliberately swallows a failing prompt and
# proceeds with no policy -- which is exactly the expected result -- so with
# "not interactive" deleted from the guard both stayed green. An unwanted prompt
# is now RECORDED and answered, so it changes the decision instead of hiding in
# the very except clause under test.

def test_no_engine_running_means_no_prompt_and_no_policy():
    calls = []
    assert decide_stop(engine_running=False, interactive=True,
                       ask=_answers(StopChoice.CANCEL, calls)) == (
        StopDecision(proceed=True, policy=None, asked=False))
    assert calls == [], "a prompt was shown with no engine to stop"


@pytest.mark.parametrize("engine_running", [True, False])
def test_a_close_nobody_started_never_shows_a_prompt(engine_running):
    """OS session end, SIGINT/SIGTERM: a modal box would block the shutdown for
    ever. No policy is sent -- the engine's engine.shutdown_offers decides."""
    calls = []
    assert decide_stop(engine_running=engine_running, interactive=False,
                       ask=_answers(StopChoice.CANCEL, calls)) == (
        StopDecision(proceed=True, policy=None, asked=False))
    assert calls == [], "a prompt was shown where nobody could answer it"


@pytest.mark.parametrize(("choice", "decision"), [
    (StopChoice.KEEP, StopDecision(proceed=True, policy=POLICY_KEEP, asked=True)),
    (StopChoice.CANCEL, StopDecision(proceed=True, policy=POLICY_CANCEL, asked=True)),
    (StopChoice.DONT_STOP, StopDecision(proceed=False, policy=None, asked=True)),
])
def test_the_operators_answer_decides(choice, decision):
    calls = []
    assert decide_stop(engine_running=True, interactive=True,
                       ask=_answers(choice, calls)) == decision
    assert calls == [choice], "the prompt is shown exactly once"


def test_an_answer_that_is_no_choice_at_all_calls_the_stop_off():
    assert decide_stop(engine_running=True, interactive=True,
                       ask=lambda: None).proceed is False
    assert decide_stop(engine_running=True, interactive=True,
                       ask=lambda: "keep").proceed is False


def test_a_prompt_that_raises_does_not_trap_the_close(caplog):
    caplog.set_level(logging.DEBUG)

    def broken():
        raise RuntimeError("no display")

    decision = decide_stop(engine_running=True, interactive=True, ask=broken)
    assert decision == StopDecision(proceed=True, policy=None, asked=False)
    assert any("engine.shutdown_offers" in r.getMessage() for r in caplog.records)


@pytest.mark.parametrize(("requested", "keep_supported", "sent"), [
    (None, True, None), (None, False, None),
    ("cancel", True, POLICY_CANCEL), ("cancel", False, POLICY_CANCEL),
    ("keep", True, POLICY_KEEP),
    # An engine that predates the policy cancels whatever the line says; the
    # line is withheld, never downgraded to "cancel" as if that were the ask.
    ("keep", False, None),
    ("kep", True, None),
])
def test_keep_reaches_only_an_engine_that_can_honour_it(requested, keep_supported, sent):
    assert policy_to_send(requested, keep_supported=keep_supported) == sent


def test_the_noninteractive_latch_is_set_once_and_stays():
    stop_offers.reset_noninteractive_quit()
    try:
        assert stop_offers.noninteractive_quit_reason() is None
        stop_offers.mark_noninteractive_quit("SIGTERM")
        stop_offers.mark_noninteractive_quit("OS session end")
        assert stop_offers.noninteractive_quit_reason() == "SIGTERM"
    finally:
        stop_offers.reset_noninteractive_quit()


def test_gui_main_marks_a_signal_and_a_session_end_before_quitting():
    """LINT-CLASS, disclosed: gui/main.py's handlers are not callable without a
    QApplication and a real signal. This pins the wiring over the source text:
    the quit is marked BEFORE app.quit() closes the windows, and the session
    manager's commitDataRequest is connected."""
    source = (_REPO / "gui" / "main.py").read_text(encoding="utf-8")
    handler = source[source.index("def _shutdown_handler("):]
    handler = handler[:handler.index("signal.signal(signal.SIGINT")]
    assert handler.index("stop_offers.mark_noninteractive_quit(") < handler.index("app.quit()")
    assert "app.commitDataRequest.connect(_on_session_ending)" in source
    session = source[source.index("def _on_session_ending("):]
    assert "stop_offers.mark_noninteractive_quit(" in session[:400]


# --------------------------------------------------------------------------- #
# 3. What would be left resting
# --------------------------------------------------------------------------- #

CONFIG = {
    "strategy": {"offer_expiry_secs": 86400},
    "pairs": [
        {"name": "XCH/DBX"},
        {"name": "XCH/BYC", "offer_expiry_secs_override": 172800},
        {"name": "XCH/wUSDC.b", "offer_expiry_secs_override": 0},
    ],
}


@pytest.mark.parametrize(("pair", "secs"), [
    ("XCH/DBX", 86400),          # inherits the global
    ("XCH/BYC", 172800),         # its own override
    ("XCH/wUSDC.b", 0),          # 0 BINDS: never expire, not "inherit"
    ("XCH/unknown", 86400),      # a pair the config does not list: the global
])
def test_effective_expiry_is_the_engines_rule(pair, secs):
    assert effective_expiry_secs(CONFIG, pair) == secs


@pytest.mark.parametrize("config", [None, {}, {"strategy": None}, {"strategy": {}},
                                    {"strategy": {"offer_expiry_secs": "soon"}},
                                    {"strategy": {"offer_expiry_secs": -5}},
                                    {"strategy": {"offer_expiry_secs": True}}])
def test_effective_expiry_is_zero_when_nothing_usable_is_configured(config):
    assert effective_expiry_secs(config, "XCH/DBX") == 0


def test_summary_counts_pairs_and_takes_the_latest_expiry():
    rows = [
        ("XCH/DBX", "pending", "2026-09-20 10:00:00"),          # +1d -> 09-21 10:00
        ("XCH/DBX", "pending", "2026-09-20 12:30:00"),          # +1d -> 09-21 12:30
        ("XCH/BYC", "pending", "2026-09-19 08:00:00"),          # +2d -> 09-21 08:00
        ("XCH/BYC", "cancel_pending", "2026-09-20 13:00:00"),   # not resting
        ("XCH/DBX", "cancelled", "2026-09-20 14:00:00"),        # not on the book
    ]
    summary = summarise_resting_offers(rows, CONFIG)
    assert summary.resting == 3
    assert summary.cancel_in_flight == 1
    assert summary.per_pair == (("XCH/BYC", 1), ("XCH/DBX", 2))
    assert summary.latest_expiry == datetime(2026, 9, 21, 12, 30, tzinfo=timezone.utc)
    assert summary.without_expiry == 0
    assert summary.expiry_unknown == 0


def test_a_cancel_pending_offer_never_sets_the_latest_expiry():
    rows = [
        ("XCH/DBX", "pending", "2026-09-20 10:00:00"),
        ("XCH/BYC", "cancel_pending", "2026-09-25 10:00:00"),
    ]
    assert summarise_resting_offers(rows, CONFIG).latest_expiry == datetime(
        2026, 9, 21, 10, 0, tzinfo=timezone.utc)


def test_offers_without_an_expiry_are_counted_not_dated():
    rows = [
        ("XCH/wUSDC.b", "pending", "2026-09-20 10:00:00"),
        ("XCH/DBX", "pending", "not a timestamp"),
    ]
    summary = summarise_resting_offers(rows, CONFIG)
    assert (summary.resting, summary.without_expiry, summary.expiry_unknown) == (2, 1, 1)
    assert summary.latest_expiry is None

    no_expiry_anywhere = summarise_resting_offers(
        [("XCH/DBX", "pending", "2026-09-20 10:00:00")], {"strategy": {}})
    assert no_expiry_anywhere.without_expiry == 1
    assert no_expiry_anywhere.latest_expiry is None


def _offer_log(path: Path, rows) -> Path:
    conn = sqlite3.connect(path)
    conn.execute(
        "CREATE TABLE offer_log (offer_id TEXT, pair_name TEXT, status TEXT, created_at TEXT)")
    conn.executemany("INSERT INTO offer_log VALUES (?, ?, ?, ?)", rows)
    conn.commit()
    conn.close()
    return path


def test_resting_rows_are_read_without_touching_the_database(tmp_path):
    db = _offer_log(tmp_path / "xop_trader.db", [
        ("0x1", "XCH/DBX", "pending", "2026-09-20 10:00:00"),
        ("0x2", "XCH/DBX", "cancel_pending", "2026-09-20 11:00:00"),
        ("0x3", "XCH/DBX", "cancelled", "2026-09-20 12:00:00"),
        ("0x4", "XCH/BYC", "filled", "2026-09-20 12:00:00"),
    ])
    before = db.read_bytes()
    rows = read_resting_rows(db)
    assert sorted(tuple(r) for r in rows) == [
        ("XCH/DBX", "cancel_pending", "2026-09-20 11:00:00"),
        ("XCH/DBX", "pending", "2026-09-20 10:00:00"),
    ]
    assert db.read_bytes() == before
    assert sorted(p.name for p in tmp_path.iterdir()) == ["xop_trader.db"], (
        "a read-only prompt query left a journal or WAL beside the engine's database")


def test_an_unreadable_database_is_none_never_an_empty_book(tmp_path):
    """None and [] are opposite answers: "cannot see" vs "nothing resting"."""
    missing = tmp_path / "nope.db"
    assert read_resting_rows(missing) is None
    assert not missing.exists(), "a read-only open must not create the file"
    assert read_resting_rows(None) is None

    no_table = tmp_path / "empty.db"
    sqlite3.connect(no_table).close()
    assert read_resting_rows(no_table) is None

    garbage = tmp_path / "garbage.db"
    garbage.write_bytes(b"this is not a sqlite database at all" * 40)
    assert read_resting_rows(garbage) is None

    assert read_resting_rows(_offer_log(tmp_path / "flat.db", [])) == []


def _prompt(summary, **overrides):
    kwargs = dict(default_policy=POLICY_CANCEL, keep_supported=True, closing=False)
    kwargs.update(overrides)
    return build_prompt_text(summary, **kwargs)


def test_the_prompt_shows_the_count_the_pairs_and_the_latest_expiry():
    latest = datetime(2026, 9, 21, 12, 30, tzinfo=timezone.utc)
    text = _prompt(RestingSummary(
        resting=14, cancel_in_flight=2, per_pair=(("XCH/BYC", 6), ("XCH/DBX", 8)),
        latest_expiry=latest))
    assert text.title == "Stop Trading"
    assert "14 offer(s) are resting on the book (XCH/BYC 6, XCH/DBX 8)." in text.text
    assert "2 more already have a cancel in flight" in text.text
    assert latest.astimezone().strftime("%Y-%m-%d %H:%M") in text.informative
    assert "engine.shutdown_offers = cancel" in text.informative
    assert "KEEP IS NOT AVAILABLE" not in text.informative


def test_the_prompt_says_plainly_when_offers_carry_no_expiry():
    text = _prompt(RestingSummary(resting=3, per_pair=(("XCH/DBX", 3),), without_expiry=3))
    assert "3 of them are on a pair with NO on-chain expiry" in text.informative
    assert "On-chain expiry is on" not in text.informative


def test_the_prompt_never_shows_zero_for_a_database_it_could_not_read():
    text = _prompt(None)
    assert "could not be read" in text.text
    assert "No offers are resting" not in text.text
    assert "No offers are resting on the book." in _prompt(RestingSummary()).text


def test_the_close_prompt_says_what_dont_stop_means_there():
    closing = _prompt(RestingSummary(resting=1, per_pair=(("XCH/DBX", 1),)), closing=True)
    assert closing.title == "Close XOPTrader"
    assert "the window stays open" in closing.informative
    assert "the window stays open" not in _prompt(RestingSummary()).informative


def test_the_prompt_names_the_default_and_an_engine_that_cannot_keep():
    text = _prompt(RestingSummary(), default_policy=POLICY_KEEP, keep_supported=False)
    assert "engine.shutdown_offers = keep" in text.informative
    assert "KEEP IS NOT AVAILABLE" in text.informative


@pytest.mark.parametrize(("default", "keep_supported", "choice"), [
    (POLICY_CANCEL, True, StopChoice.CANCEL),
    (POLICY_KEEP, True, StopChoice.KEEP),
    (POLICY_KEEP, False, StopChoice.CANCEL),   # never preselect a disabled button
    ("kep", True, StopChoice.CANCEL),
])
def test_the_preselected_choice_is_the_config_default(default, keep_supported, choice):
    assert default_choice(default, keep_supported=keep_supported) is choice


# --------------------------------------------------------------------------- #
# 4. The Settings save rule (pure half; the widget half is
#    tests/test_stop_offers_settings.py)
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize(("base", "selected", "populated", "written"), [
    # Untouched page: nothing is added, nothing is changed.
    (None, "cancel", None, None),
    ({}, "cancel", None, None),
    ({"shutdown_offers": "keep"}, "keep", "keep", {"shutdown_offers": "keep"}),
    # Untouched page, value edited on disk since it loaded: the disk wins.
    ({"shutdown_offers": "keep"}, "cancel", None, {"shutdown_offers": "keep"}),
    ({"shutdown_offers": "cancel"}, "keep", "keep", {"shutdown_offers": "cancel"}),
    # The operator changed the dropdown: that one key is set.
    (None, "keep", None, {"shutdown_offers": "keep"}),
    ({"shutdown_offers": "keep"}, "cancel", "keep", {"shutdown_offers": "cancel"}),
    ({"shutdown_offers": "cancel"}, "keep", "cancel", {"shutdown_offers": "keep"}),
    # ...and every other key in the section survives, in order.
    ({"future_key": 1, "shutdown_offers": "cancel", "z": [1]}, "keep", "cancel",
     {"future_key": 1, "shutdown_offers": "keep", "z": [1]}),
    ({"future_key": 1}, "cancel", None, {"future_key": 1}),
    # A value nobody can read stops the engine booting: the page's value replaces it.
    ({"shutdown_offers": "kep"}, "cancel", None, {"shutdown_offers": "cancel"}),
])
def test_merge_shutdown_offers_patches_one_key(base, selected, populated, written):
    result = merge_shutdown_offers(base, selected=selected, populated=populated)
    assert result == written
    if written is not None:
        assert list(result) == list(written), "key order changed"
    if isinstance(base, dict):
        assert result is not base, "the on-disk mapping must not be mutated"


# --------------------------------------------------------------------------- #
# 5. The Qt prompt (built, never exec()'d)
# --------------------------------------------------------------------------- #

@pytest.fixture(scope="module")
def qapp():
    pytest.importorskip("PySide6")
    from PySide6.QtWidgets import QApplication

    yield QApplication.instance() or QApplication([])


def _dialog(qapp, *, default_policy=POLICY_CANCEL, keep_supported=True):
    from gui.widgets.stop_engine_dialog import StopEngineDialog

    return StopEngineDialog(
        None, _prompt(RestingSummary(resting=2, per_pair=(("XCH/DBX", 2),))),
        default_policy=default_policy, keep_supported=keep_supported)


def test_the_dialog_has_exactly_the_three_choices(qapp):
    from gui.widgets import stop_engine_dialog as dlg

    dialog = _dialog(qapp)
    try:
        # buttons() is ordered by the platform's button-role layout, not by
        # insertion, so compare the set.
        assert sorted(b.text() for b in dialog.buttons()) == sorted([
            dlg.KEEP_LABEL, dlg.CANCEL_LABEL, dlg.DONT_STOP_LABEL])
        assert (dlg.KEEP_LABEL, dlg.CANCEL_LABEL, dlg.DONT_STOP_LABEL) == (
            "Keep offers on the book", "Cancel all offers", "Don't stop")
        assert "2 offer(s) are resting" in dialog.text()
    finally:
        dialog.deleteLater()


@pytest.mark.parametrize(("default_policy", "keep_supported", "preselected"), [
    (POLICY_CANCEL, True, StopChoice.CANCEL),
    (POLICY_KEEP, True, StopChoice.KEEP),
    (POLICY_KEEP, False, StopChoice.CANCEL),
])
def test_the_dialog_preselects_the_config_default_and_escape_means_dont_stop(
        qapp, default_policy, keep_supported, preselected):
    dialog = _dialog(qapp, default_policy=default_policy, keep_supported=keep_supported)
    try:
        assert dialog.defaultButton() is dialog.button_for(preselected)
        assert dialog.escapeButton() is dialog.button_for(StopChoice.DONT_STOP)
        assert dialog.button_for(StopChoice.KEEP).isEnabled() is keep_supported
    finally:
        dialog.deleteLater()


@pytest.mark.parametrize("choice", list(StopChoice))
def test_the_clicked_button_is_the_answer(qapp, choice):
    dialog = _dialog(qapp)
    try:
        dialog.button_for(choice).click()
        assert dialog.choice() is choice
    finally:
        dialog.deleteLater()


def test_a_dialog_nobody_clicked_means_dont_stop(qapp):
    dialog = _dialog(qapp)
    try:
        assert dialog.choice() is StopChoice.DONT_STOP
        dialog.reject()  # Escape / the title-bar X
        assert dialog.choice() is StopChoice.DONT_STOP
    finally:
        dialog.deleteLater()


# --------------------------------------------------------------------------- #
# 6. EngineBridge: the policy reaches the request
# --------------------------------------------------------------------------- #

class ScriptedEngine:
    """A Popen double whose wait() plays *script(proc, timeout)*."""

    def __init__(self, pid, script):
        self.pid = pid
        self.returncode = None
        self._script = script
        self.terminated = False

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        self._script(self, timeout)
        return self.returncode

    def terminate(self):
        self.terminated = True

    def kill(self):  # pragma: no cover - never reached by these scripts
        raise AssertionError("kill() in a scripted graceful stop")


class _FakeSignal:
    def __init__(self):
        self.messages = []

    def emit(self, message):
        self.messages.append(message)


def _flag(tmp_path):
    return tmp_path / "data" / shutdown_flag.FLAG_NAME


@pytest.fixture
def bridge_factory(tmp_path, monkeypatch):
    pytest.importorskip("PySide6")
    from gui.services.engine_bridge import EngineBridge

    errors = _FakeSignal()
    # __new__ without QObject.__init__: a real Signal cannot be touched.
    monkeypatch.setattr(EngineBridge, "error", errors, raising=False)

    def make(*, keep_supported):
        seen = {}

        def engine_honours_it(proc, _timeout):
            seen["request"] = _flag(tmp_path).read_bytes().decode("ascii")
            _flag(tmp_path).unlink()  # the engine consumes it
            proc.returncode = 0

        proc = ScriptedEngine(ENGINE_PID, engine_honours_it)
        bridge = EngineBridge.__new__(EngineBridge)  # stop logic only, no Qt init
        bridge._db_path = tmp_path / "data" / "xop_trader.db"
        bridge._engine_process = proc
        bridge._engine_log_fh = None
        bridge._engine_log_path = None
        bridge._engine_launch_dir = None
        bridge._engine_keep_supported = keep_supported
        return bridge, proc, seen, errors

    return make


@pytest.mark.parametrize(("policy", "expected"), [
    ("keep", shutdown_flag.OffersRequest.KEEP),
    ("cancel", shutdown_flag.OffersRequest.CANCEL),
    (None, shutdown_flag.OffersRequest.UNSPECIFIED),
])
def test_stop_engine_writes_the_operators_answer_into_the_request(
        bridge_factory, caplog, policy, expected):
    caplog.set_level(logging.DEBUG)
    bridge, proc, seen, errors = bridge_factory(keep_supported=True)
    bridge.stop_engine(offers_policy=policy)

    parsed = shutdown_flag.parse_shutdown_request(seen["request"])
    assert (parsed.kind, parsed.pid, parsed.offers) == (
        shutdown_flag.RequestKind.ADDRESSED, ENGINE_PID, expected)
    assert not proc.terminated and bridge._engine_process is None
    assert errors.messages == []
    wrote = [r.getMessage() for r in caplog.records if "Wrote shutdown.flag" in r.getMessage()]
    assert len(wrote) == 1
    assert {"keep": "offers=keep", "cancel": "offers=cancel",
            None: "no offers line"}[policy] in wrote[0]


def test_a_stop_with_no_answer_is_byte_identical_to_the_pre_policy_request(bridge_factory):
    """An old engine, and every stop nobody answered, must see what it always saw."""
    bridge, _proc, seen, _errors = bridge_factory(keep_supported=True)
    bridge._stop_engine_process()
    lines = seen["request"].splitlines()
    assert lines[0] == shutdown_flag.FORMAT_LINE and lines[1] == f"pid={ENGINE_PID}"
    assert len(lines) == 4 and not any(line.startswith("offers=") for line in lines)


def test_keep_is_refused_not_downgraded_for_an_engine_that_would_cancel(
        bridge_factory, tmp_path, caplog):
    caplog.set_level(logging.DEBUG)
    bridge, proc, seen, errors = bridge_factory(keep_supported=False)
    bridge.stop_engine(offers_policy="keep")

    assert "request" not in seen, "a stop request was written for an engine that would cancel"
    assert not _flag(tmp_path).exists()
    assert bridge._engine_process is proc, "the engine must keep running"
    assert len(errors.messages) == 1 and "NOT stopped" in errors.messages[0]


def test_the_close_prompts_answer_rides_on_the_bridge_until_shutdown(bridge_factory):
    from gui.services.engine_bridge import EngineBridge

    bridge, _proc, seen, _errors = bridge_factory(keep_supported=True)
    bridge.set_close_offers_policy("keep")
    # shutdown() also stops services this double does not have; drive the one
    # line of it that matters here.
    EngineBridge._stop_engine_process(
        bridge, offers_policy=getattr(bridge, "_close_offers_policy", None))
    assert shutdown_flag.parse_shutdown_request(
        seen["request"]).offers is shutdown_flag.OffersRequest.KEEP


def test_shutdown_passes_the_held_policy_to_the_stop():
    """LINT-CLASS, disclosed: EngineBridge.shutdown() stops five services before
    the engine, none of which exist on the double above."""
    source = (_REPO / "gui" / "services" / "engine_bridge.py").read_text(encoding="utf-8")
    body = source[source.index("    def shutdown(self) -> None:"):]
    body = body[:body.index("    # ===")]
    assert ('self._stop_engine_process(\n            offers_policy=getattr(self, '
            '"_close_offers_policy", None))') in body.replace("\r\n", "\n")


def test_a_failed_launch_probe_is_asked_once_more_and_only_once(bridge_factory, monkeypatch):
    """The --help probe has a 2 s timeout. A false "no" on a busy machine would
    send the operator back to hard-killing the engine to keep offers, so a "no"
    is asked a second time when the answer is first needed -- and never a third."""
    from gui.services.engine_bridge import EngineBridge

    probes = []
    answers = [True]

    def fake_probe(self, engine_path, flag):
        probes.append((engine_path, flag))
        return answers[0]

    monkeypatch.setattr(EngineBridge, "_engine_supports_flag", fake_probe)

    bridge, _proc, seen, _errors = bridge_factory(keep_supported=False)
    bridge._engine_binary_path = Path("xop_trader.exe")
    assert bridge.engine_supports_keep_offers is True
    assert bridge.engine_supports_keep_offers is True
    assert probes == [(Path("xop_trader.exe"), stop_offers.ENGINE_HELP_TOKEN)]
    bridge.stop_engine(offers_policy="keep")
    assert shutdown_flag.parse_shutdown_request(
        seen["request"]).offers is shutdown_flag.OffersRequest.KEEP

    # A second "no" stands, and is not asked again.
    probes.clear()
    answers[0] = False
    stubborn, _proc, _seen, _errors = bridge_factory(keep_supported=False)
    stubborn._engine_binary_path = Path("old_engine.exe")
    assert stubborn.engine_supports_keep_offers is False
    assert stubborn.engine_supports_keep_offers is False
    assert len(probes) == 1

    # No binary on record (a bridge that launched nothing): never probed.
    probes.clear()
    nothing, _proc, _seen, _errors = bridge_factory(keep_supported=False)
    assert nothing.engine_supports_keep_offers is False
    assert probes == []


def test_a_close_with_keep_is_withheld_for_an_engine_that_cannot_keep(bridge_factory):
    bridge, _proc, _seen, _errors = bridge_factory(keep_supported=False)
    bridge.set_close_offers_policy("keep")
    assert bridge._close_offers_policy is None
    bridge.set_close_offers_policy("cancel")
    assert bridge._close_offers_policy == "cancel"
