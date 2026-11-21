"""The read-only Circuit page (#225): its switch, its rounds of reads, what it shows.

The page's promise is that it asks Circuit's API only while it is switched on
AND open, so the tests count the client factory's calls and the requests made,
not only what is visible. The answers are the ones the live server gave on
2026-11-21 (gui/services/circuit/tests/fixtures).
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402
from PySide6.QtCore import QCoreApplication, QEvent, QSettings  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from gui.services.circuit import poll  # noqa: E402
from gui.services.circuit.api import CircuitApi  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
FIXTURES = REPO / "gui" / "services" / "circuit" / "tests" / "fixtures"
#: When the fixtures were read (2026-11-21 18:22:40 UTC).
NOW = 1_795_285_360


@pytest.fixture(scope="module")
def app():
    instance = QApplication.instance() or QApplication(sys.argv)
    yield instance


@pytest.fixture
def real_circuit_setting():
    """Put the operator's own "circuit/enabled" back, whatever a test wrote."""
    settings = QSettings("XOP", "XOPTrader")
    settings.sync()
    saved = settings.value("circuit/enabled")
    yield
    restore = QSettings("XOP", "XOPTrader")
    if saved is None:
        restore.remove("circuit/enabled")
    else:
        restore.setValue("circuit/enabled", saved)
    restore.sync()


class FixtureServer:
    """Answers each page read with the saved live answer; counts every request."""

    def __init__(self, fail: dict[str, tuple[int, Any]] | None = None) -> None:
        self.answers = {
            read.path: (200, json.loads((FIXTURES / f"{read.name}.json").read_text("utf-8")))
            for read in poll.PAGE_READS
        }
        self.answers.update(fail or {})
        self.calls: list[str] = []

    def __call__(self, method, url, body, headers, timeout_s):
        path = "/" + url.split("://", 1)[1].split("/", 1)[1]
        self.calls.append(path)
        status, payload = self.answers[path]
        return status, {}, json.dumps(payload).encode()


class Factory:
    """The page's client factory: counts its calls, serves FixtureServer."""

    def __init__(self, server: FixtureServer) -> None:
        self.server = server
        self.made = 0

    def __call__(self, stop: threading.Event) -> CircuitApi:
        self.made += 1

        def sleep(seconds: float) -> None:
            if stop.wait(seconds):
                raise poll.Stopped()

        return CircuitApi(
            "https://api.example.test", min_interval_s=0, transport=self.server, sleep=sleep
        )


class Clock:
    def __init__(self) -> None:
        self.now = float(NOW)

    def __call__(self) -> float:
        return self.now


def _wait_until(app, predicate, seconds: float = 5.0) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        app.processEvents()
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("timed out")


def _page(server: FixtureServer | None = None, clock: Clock | None = None):
    from gui.widgets.circuit import CircuitWidget

    factory = Factory(server or FixtureServer())
    page = CircuitWidget(client_factory=factory, wall=clock or Clock())
    return page, factory


def _close(page, app) -> None:
    page.stop_background_work()
    page.hide()
    page.deleteLater()
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    app.processEvents()


# -- the page on its own ------------------------------------------------------------


def test_building_the_page_asks_nothing(app) -> None:
    page, factory = _page()
    try:
        page.show()
        app.processEvents()
        assert factory.made == 0 and factory.server.calls == []
        assert not page.is_enabled() and not page.fetching()
        assert "Switched off" in page._status.text()
        assert not page.refresh()
        assert not page._refresh_button.isEnabled()
    finally:
        _close(page, app)


def test_switched_on_but_not_open_it_still_asks_nothing(app) -> None:
    page, factory = _page()
    try:
        page.set_enabled(True)
        app.processEvents()
        assert factory.made == 0 and not page.fetching()
        assert page._status.text() == "Not read yet: the page reads when it is open."
    finally:
        _close(page, app)


def test_opened_it_reads_once_on_a_worker_thread_and_shows_the_answers(app) -> None:
    page, factory = _page()
    try:
        page.set_enabled(True)
        page.show()
        _wait_until(app, lambda: not page.fetching() and len(factory.server.calls) == 8)
        assert factory.made == 1
        assert factory.server.calls == [read.path for read in poll.PAGE_READS]
        assert page._status.text().startswith("Read at 18:22 UTC: 8 of 8 answered.")

        values = {key: label.text() for key, label in page._values.items()}
        assert values["price"] == "$0.94 per XCH, set at 14:25 UTC"
        assert values["oracle"] == "newest price $0.96, announced at 17:43 UTC, usable in 21 min"
        assert values["fee"].startswith("4.30% a year")
        assert values["savings"].startswith("4.00% a year")
        assert values["liquidation"] == "below 166% collateral; 13% penalty"
        assert values["collateral"] == ("67,657 XCH locked, of a 1,000,000 XCH cap (6.8%)")
        assert values["borrowed"].startswith("15,268 BYC, in 13 of 15 vaults")
        assert values["emergency"] == "off (BLOCK_ISSUANCE is 0)"
        assert page._banner.isHidden()

        from gui.widgets.circuit import _cells

        (running,) = _cells(page._auctions_running)
        assert running[0] == "5a51ec09...4096"
        assert running[3:6] == ["$1.03", "110%", "in 15 min"]
        closest = _cells(page._closest)
        assert closest[0][0] == "58b72f09...5bf1"
        assert closest[0][3:] == ["191%", "$0.81", "13.8%", "nearing liquidation"]
        assert len(closest) == 8
        assert "1 auction running" in page._liquidation_summary.text()
        assert "2 in bad debt (23.989 BYC)" in page._liquidation_summary.text()

        assert "968.519 BYC in 3 coins" in page._treasury.text()
        assert _cells(page._auctions) == [
            ["c6164f84...3a5f", "Recharge", "STANDBY", "no", "not started"]
        ]
        assert _cells(page._bills)[0][1:] == [
            "31,000,000",
            "no bill",
            "last step: implement_bill",
            "can leave governance",
        ]
        assert len(_cells(page._announcers)) == 4
        statutes = _cells(page._statutes)
        assert len(statutes) == 44 and statutes[-1] == ["43", "BLOCK_ISSUANCE", "0"]
        assert all(note.isHidden() for notes in page._notes.values() for note in notes)
    finally:
        _close(page, app)


def test_opening_it_again_within_a_minute_does_not_read_again(app) -> None:
    clock = Clock()
    page, factory = _page(clock=clock)
    try:
        page.set_enabled(True)
        page.show()
        _wait_until(app, lambda: not page.fetching() and len(factory.server.calls) == 8)
        page.hide()
        clock.now += 30
        page.show()
        app.processEvents()
        assert not page.fetching() and len(factory.server.calls) == 8
        page.hide()
        clock.now += 31
        page.show()
        _wait_until(app, lambda: not page.fetching() and len(factory.server.calls) == 16)
        assert factory.made == 1, "one client for the page's life, so its spacing holds"
    finally:
        _close(page, app)


def test_closed_it_stops_its_timers(app) -> None:
    page, _factory = _page()
    try:
        page.set_enabled(True)
        page.show()
        assert page._round_timer.isActive() and page._redraw_timer.isActive()
        _wait_until(app, lambda: not page.fetching())
        page.hide()
        assert not page._round_timer.isActive() and not page._redraw_timer.isActive()
    finally:
        _close(page, app)


def test_a_failed_read_is_noted_in_its_section(app) -> None:
    server = FixtureServer(fail={"/vaults": (503, {"detail": "maintenance"})})
    page, _factory = _page(server)
    try:
        page.set_enabled(True)
        page.show()
        _wait_until(app, lambda: not page.fetching() and len(server.calls) == 8)
        assert "7 of 8 answered. Notes in the sections below." in page._status.text()
        (note,) = page._notes["vaults"]
        assert not note.isHidden()
        assert note.text().startswith("vaults: POST /vaults: HTTP 503")
        from gui.widgets.circuit import _cells

        assert _cells(page._closest) == [["Not read yet"] + [""] * 6]
        # The other sections are filled.
        assert page._values["price"].text().startswith("$0.94")
    finally:
        _close(page, app)


def test_the_emergency_switch_is_a_banner(app) -> None:
    server = FixtureServer()
    server.answers["/statutes"][1]["implemented_statutes"]["BLOCK_ISSUANCE"] = 1
    page, _factory = _page(server)
    try:
        page.set_enabled(True)
        page.show()
        _wait_until(app, lambda: not page.fetching() and len(server.calls) == 8)
        assert not page._banner.isHidden()
        assert "emergency switch is on (BLOCK_ISSUANCE = 1)" in page._banner.text()
        assert page._values["emergency"].text() == "ON: BLOCK_ISSUANCE is 1"
    finally:
        _close(page, app)


def _garbled_statutes(good: dict, how: str) -> dict:
    if how == "statutes not an object":
        return {"status": "ok", "implemented_statutes": []}
    garbled = json.loads(json.dumps(good))
    if how == "no emergency switch":
        del garbled["implemented_statutes"]["BLOCK_ISSUANCE"]
    else:  # no status
        del garbled["status"]
    return garbled


@pytest.mark.parametrize("how", ["statutes not an object", "no emergency switch", "no status"])
def test_a_garbled_answer_keeps_the_last_good_one_and_its_banner(app, how) -> None:
    server = FixtureServer()
    server.answers["/statutes"][1]["implemented_statutes"]["BLOCK_ISSUANCE"] = 1
    page, _factory = _page(server)
    try:
        page.set_enabled(True)
        page.show()
        _wait_until(app, lambda: not page.fetching() and len(server.calls) == 8)
        assert not page._banner.isHidden()

        server.answers["/statutes"] = (200, _garbled_statutes(server.answers["/statutes"][1], how))
        assert page.refresh()
        _wait_until(app, lambda: not page.fetching() and len(server.calls) == 16)
        # Answered, but of another shape: a failed read, not a new answer.
        assert "7 of 8 answered" in page._status.text()
        assert not page._banner.isHidden(), "the emergency the last good answer showed"
        assert page._values["emergency"].text() == "ON: BLOCK_ISSUANCE is 1"
        notes = page._notes["statutes"]
        assert notes and all(
            note.text().startswith("statutes: the answer has an unexpected shape") for note in notes
        )
        from gui.widgets.circuit import _cells

        assert len(_cells(page._statutes)) == 44
    finally:
        _close(page, app)


def test_every_row_of_a_table_shows_above_a_horizontal_scroll_bar(app) -> None:
    page, factory = _page()
    tables = (
        page._auctions_running,
        page._closest,
        page._auctions,
        page._bills,
        page._announcers,
        page._statutes,
    )

    def spare(table) -> int:
        """The table's room for rows, less the rows: 0 to a few pixels, never below."""
        rows = sum(table.rowHeight(r) for r in range(table.rowCount()))
        return table.viewport().height() - rows

    try:
        page.set_enabled(True)
        page.resize(420, 1200)
        page.show()
        _wait_until(app, lambda: not page.fetching() and len(factory.server.calls) == 8)
        app.processEvents()
        assert page._closest.horizontalScrollBar().isVisible(), "narrow enough for the bar"
        assert all(0 <= spare(table) <= 4 for table in tables), [spare(t) for t in tables]

        page.resize(1800, 1200)
        app.processEvents()
        assert not page._closest.horizontalScrollBar().isVisible()
        # Wide again, the room the bar took is given back.
        assert all(0 <= spare(table) <= 4 for table in tables), [spare(t) for t in tables]
    finally:
        _close(page, app)


def test_a_first_answer_without_the_emergency_switch_says_neither_on_nor_off(app) -> None:
    server = FixtureServer()
    del server.answers["/statutes"][1]["implemented_statutes"]["BLOCK_ISSUANCE"]
    page, _factory = _page(server)
    try:
        page.set_enabled(True)
        page.show()
        _wait_until(app, lambda: not page.fetching() and len(server.calls) == 8)
        assert page._values["emergency"].text() == "-"
        assert page._banner.isHidden()
        notes = page._notes["statutes"]
        assert notes and all(
            note.text() == "statutes: the answer has an unexpected shape (KeyError: "
            "'BLOCK_ISSUANCE')"
            for note in notes
        )
    finally:
        _close(page, app)


def test_an_emergency_and_a_status_warning_both_show(app) -> None:
    server = FixtureServer()
    server.answers["/statutes"][1]["implemented_statutes"]["BLOCK_ISSUANCE"] = 1
    server.answers["/statutes"][1]["status"] = "degraded"
    page, _factory = _page(server)
    try:
        page.set_enabled(True)
        page.show()
        _wait_until(app, lambda: not page.fetching() and len(server.calls) == 8)
        lines = page._banner.text().splitlines()
        assert len(lines) == 2 and not page._banner.isHidden()
        assert "emergency switch is on (BLOCK_ISSUANCE = 1)" in lines[0]
        assert lines[1] == "The statutes answer says status 'degraded'."
    finally:
        _close(page, app)


def test_the_tables_take_keyboard_focus_and_bring_in_columns_out_of_view(app) -> None:
    from PySide6.QtCore import Qt
    from PySide6.QtTest import QTest

    page, factory = _page()
    try:
        page.set_enabled(True)
        page.resize(420, 1200)
        page.show()
        _wait_until(app, lambda: not page.fetching() and len(factory.server.calls) == 8)
        app.processEvents()
        table = page._closest
        assert table.focusPolicy() & Qt.FocusPolicy.TabFocus, "Tab reaches it"
        bar = table.horizontalScrollBar()
        assert bar.isVisible() and bar.value() == 0
        table.setFocus()
        table.setCurrentCell(0, 0)
        QTest.keyClick(table, Qt.Key.Key_End)  # to the row's last column
        app.processEvents()
        assert table.currentColumn() == table.columnCount() - 1
        assert bar.value() > 0, "the column out of view was scrolled in"
    finally:
        _close(page, app)


def test_switching_off_ends_a_round_at_its_next_wait(app) -> None:
    entered, gate = threading.Event(), threading.Event()
    server = FixtureServer()

    def transport(method, url, body, headers, timeout_s):
        if url.endswith("/statutes"):
            entered.set()
            gate.wait(5)
        return server(method, url, body, headers, timeout_s)

    from gui.widgets.circuit import CircuitWidget

    def factory(stop: threading.Event) -> CircuitApi:
        def sleep(seconds: float) -> None:
            if stop.wait(seconds):
                raise poll.Stopped()

        # Spaced, so the round has a wait to stop at after the slow answer.
        return CircuitApi(
            "https://api.example.test", min_interval_s=0.2, transport=transport, sleep=sleep
        )

    page = CircuitWidget(client_factory=factory, wall=Clock())
    try:
        page.set_enabled(True)
        page.show()
        _wait_until(app, entered.is_set)  # the round is in the middle of its second read
        page.set_enabled(False)
        gate.set()
        _wait_until(app, lambda: not page.fetching())
        # The read under way finished; the next one stopped at its wait, unasked.
        assert server.calls == ["/oracle", "/statutes"]
        assert not page._round_timer.isActive()
    finally:
        gate.set()
        _close(page, app)


def test_a_round_due_after_one_cut_short_waits_until_the_page_is_open(app) -> None:
    entered, gate = threading.Event(), threading.Event()
    server = FixtureServer()

    def transport(method, url, body, headers, timeout_s):
        if url.endswith("/statutes") and not gate.is_set():
            entered.set()
            gate.wait(5)
        return server(method, url, body, headers, timeout_s)

    from gui.widgets.circuit import CircuitWidget

    def factory(stop: threading.Event) -> CircuitApi:
        def sleep(seconds: float) -> None:
            if stop.wait(seconds):
                raise poll.Stopped()

        return CircuitApi(
            "https://api.example.test", min_interval_s=0.2, transport=transport, sleep=sleep
        )

    page = CircuitWidget(client_factory=factory, wall=Clock())
    try:
        page.set_enabled(True)
        page.show()
        _wait_until(app, entered.is_set)  # the first round is in its second read
        page.set_enabled(False)  # it ends at its next wait...
        page.set_enabled(True)  # ...and on again, a new round is due once it has
        assert page._round_due
        page.hide()  # closed before the first round has ended
        gate.set()
        _wait_until(app, lambda: not page.fetching())
        app.processEvents()
        assert server.calls == ["/oracle", "/statutes"], "closed, no round starts"
        assert "(stopped before the end)" in page._status.text()

        # Opened again within the minute: a round cut short is not a fresh read.
        page.show()
        _wait_until(app, lambda: not page.fetching() and len(server.calls) == 10)
        assert server.calls[2:] == [read.path for read in poll.PAGE_READS]
    finally:
        gate.set()
        _close(page, app)


def test_closing_the_app_joins_the_round(app) -> None:
    entered, gate = threading.Event(), threading.Event()
    server = FixtureServer()

    def transport(method, url, body, headers, timeout_s):
        entered.set()
        gate.wait(5)
        return server(method, url, body, headers, timeout_s)

    from gui.widgets.circuit import CircuitWidget

    page = CircuitWidget(
        client_factory=lambda stop: CircuitApi(
            "https://api.example.test", min_interval_s=0, transport=transport
        ),
        wall=Clock(),
    )
    page.set_enabled(True)
    page.show()
    assert page.fetching()
    assert entered.wait(5), "the round never asked"
    threading.Timer(0.2, gate.set).start()
    page.stop_background_work(timeout_ms=3000)
    assert not page.fetching()
    assert server.calls == ["/oracle"], "the round ended after the answer it was waiting for"
    _close(page, app)


#: A page whose first read never answers in time, closed as the app closes it.
def test_switching_off_mid_request_ends_the_round_at_once(app) -> None:
    from gui.widgets.circuit import CircuitWidget

    entered, gate = threading.Event(), threading.Event()
    server = FixtureServer()

    def transport(method, url, body, headers, timeout_s):
        entered.set()
        gate.wait(10)
        return server(method, url, body, headers, timeout_s)

    # The page's own client: each request runs on a daemon thread of its own.
    page = CircuitWidget(
        client_factory=lambda stop: poll.page_client(stop, transport=transport), wall=Clock()
    )
    try:
        page.set_enabled(True)
        page.show()
        assert entered.wait(5), "the round never asked"
        told = time.monotonic()
        page.set_enabled(False)
        _wait_until(app, lambda: not page.fetching())
        assert time.monotonic() - told < 1, "the round waited on the request"
        assert page._errors == {} and page._answers == {}, "a dropped request is no failed read"
    finally:
        gate.set()
        _close(page, app)


class _Deaf:
    """A client whose reads do not hear the page's stop: only the gate ends them."""

    def __init__(self, entered: threading.Event, gate: threading.Event) -> None:
        self.entered, self.gate = entered, gate
        self.asked: list[str] = []

    def get(self, path: str) -> Any:
        self.asked.append(path)
        self.entered.set()
        self.gate.wait(10)
        return {}

    def post(self, path: str, body: Any = None) -> Any:
        return self.get(path)


def test_a_thread_that_outlasts_the_close_wait_is_left_to_end_not_killed(app, monkeypatch) -> None:
    import shiboken6
    from PySide6.QtCore import QThread

    from gui.widgets import circuit as circuit_mod

    killed: list[Any] = []
    # terminate() can land while the thread holds Python's lock and hang the close.
    monkeypatch.setattr(QThread, "terminate", lambda self: killed.append(self))
    entered, gate = threading.Event(), threading.Event()
    clients: list[_Deaf] = []

    def factory(stop: threading.Event) -> _Deaf:
        clients.append(_Deaf(entered, gate))
        return clients[-1]

    page = circuit_mod.CircuitWidget(client_factory=factory, wall=Clock())
    thread = None
    try:
        page.set_enabled(True)
        page.show()
        assert entered.wait(5), "the round never asked"
        thread = page._thread
        page.stop_background_work(timeout_ms=100)
        assert not killed, "a thread running Python was terminated"
        assert not page.fetching()
        assert thread.isRunning(), "it is left to end on its own"
        assert thread.parent() is None, "as the page's child it would be destroyed running"
        assert any(kept is thread for kept, _worker in circuit_mod._ABANDONED)

        gate.set()
        assert thread.wait(5000), "it ends once its read does"
        app.processEvents()
        assert clients[0].asked == ["/oracle"], "it stopped after the read it was in"
        assert page._answers == {} and page._errors == {}, "nothing it did later reached the page"

        # A later round has a client and a stop flag of its own.
        assert page.refresh()
        _wait_until(app, lambda: not page.fetching())
        assert len(clients) == 2 and len(clients[1].asked) == 8
        assert clients[0].asked == ["/oracle"], "the abandoned round did not start again"
    finally:
        gate.set()
        if thread is not None and shiboken6.isValid(thread):
            thread.wait(5000)
        kept = circuit_mod._ABANDONED
        kept[:] = [pair for pair in kept if pair[0] is not thread]
        _close(page, app)


_CLOSING = """
import os, sys, threading, time
os.environ["QT_QPA_PLATFORM"] = "offscreen"
import shiboken6
from PySide6.QtWidgets import QApplication
from gui.services.circuit import poll
from gui.widgets import circuit

app = QApplication(sys.argv)
entered, gate = threading.Event(), threading.Event()

def stuck(method, url, body, headers, timeout_s):
    entered.set()
    gate.wait(30)  # far past the page's wait
    raise TimeoutError("no answer")

class Deaf:
    def get(self, path):
        entered.set()
        gate.wait(30)
        return {}

    def post(self, path, body=None):
        return self.get(path)

if sys.argv[1] == "stuck request":  # the page's own client
    factory = lambda stop: poll.page_client(stop, transport=stuck)
else:  # a client that does not hear the stop: the page's thread itself is held
    factory = lambda stop: Deaf()
page = circuit.CircuitWidget(client_factory=factory)
page.set_enabled(True)
page.show()
if not entered.wait(5):
    sys.exit("the round never asked")
told = time.monotonic()
page.stop_background_work(timeout_ms=1000)
print(f"stopped in {time.monotonic() - told:.2f} s", flush=True)
if page.fetching():
    sys.exit("the round still counts as running")
# As the app's exit does when its window goes: the page destroyed at once, with
# its children. A QThread destroyed while it runs aborts the process.
shiboken6.delete(page)
print("closed", flush=True)
if sys.argv[1] == "deaf client":
    gate.set()  # its read ends, and the thread left running ends on its own
    for thread, _worker in circuit._ABANDONED:
        if not thread.wait(5000):
            sys.exit("the thread left running did not end")
    print("ended", flush=True)
"""


def _close_in_a_child(case: str) -> tuple[subprocess.CompletedProcess, float]:
    # In a child process: a QThread destroyed while it runs aborts the whole
    # process (qFatal), which would take this test run with it.
    started = time.monotonic()
    ran = subprocess.run(  # noqa: S603 -- this interpreter, our own code, no shell
        [sys.executable, "-c", _CLOSING, case],
        cwd=REPO,
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    return ran, time.monotonic() - started


def test_closing_the_app_mid_request_stops_at_once_and_exits_cleanly() -> None:
    ran, took = _close_in_a_child("stuck request")
    assert ran.returncode == 0, ran.stderr
    assert "closed" in ran.stdout
    stopped = float(ran.stdout.split("stopped in ", 1)[1].split(" s", 1)[0])
    assert stopped < 0.9, f"closing waited on the stuck request ({stopped} s)"
    assert "did not stop" not in ran.stderr
    assert took < 20, f"exiting waited on the stuck request ({took:.1f} s)"


def test_closing_the_app_leaves_a_thread_that_will_not_stop_to_end_on_its_own() -> None:
    ran, _took = _close_in_a_child("deaf client")
    assert ran.returncode == 0, ran.stderr
    assert "closed" in ran.stdout and "ended" in ran.stdout
    assert "did not stop in 1000 ms; it is left to end on its own" in ran.stderr


# -- the switch ----------------------------------------------------------------------


def test_the_loader_defaults_to_on(real_circuit_setting) -> None:
    from gui.widgets.settings import load_circuit_enabled

    QSettings("XOP", "XOPTrader").remove("circuit/enabled")
    assert load_circuit_enabled() is True


@pytest.mark.parametrize(
    ("stored", "expected"),
    [
        (True, True),
        (False, False),
        ("true", True),
        ("1", True),
        ("on", True),
        ("yes", True),
        ("false", False),
        ("0", False),
        ("off", False),
        ("no", False),
        # Unreadable is on, the default.
        ("sideways", True),
        ("", True),
    ],
)
def test_the_loader_reads_every_shape(stored, expected, real_circuit_setting) -> None:
    from gui.widgets.settings import load_circuit_enabled

    settings = QSettings("XOP", "XOPTrader")
    settings.setValue("circuit/enabled", stored)
    settings.sync()
    assert load_circuit_enabled() is expected


def test_the_subsystems_box_is_on_by_default_and_remembers(app, monkeypatch, real_circuit_setting):
    from gui.widgets import settings as settings_mod

    QSettings("XOP", "XOPTrader").remove("circuit/enabled")
    monkeypatch.setattr(settings_mod, "load_permuto_enabled", lambda: False)
    page = settings_mod.SettingsWidget()
    try:
        seen: list[bool] = []
        page.circuit_enabled_changed.connect(seen.append)
        assert page._circuit_enabled_box.isChecked()
        page._circuit_enabled_box.setChecked(False)
        assert seen == [False] and settings_mod.load_circuit_enabled() is False
        page._circuit_enabled_box.setChecked(True)
        assert seen == [False, True] and settings_mod.load_circuit_enabled() is True
    finally:
        page.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        app.processEvents()


# -- in the main window ------------------------------------------------------------


def _main_window(monkeypatch, *, enabled: bool | None):
    """The main window, its page's client counted; ``enabled`` None reads the real setting."""
    from gui.widgets import circuit as circuit_mod
    from gui.widgets import settings as settings_mod

    made: list[int] = []

    def page_client(stop, **_kwargs):
        made.append(1)
        return Factory(FixtureServer())(stop)

    monkeypatch.setattr(circuit_mod.poll, "page_client", page_client)
    monkeypatch.setattr(settings_mod, "load_permuto_enabled", lambda: False)
    monkeypatch.setattr(settings_mod, "load_startup_states", lambda: ("adopt", "off"))
    monkeypatch.setattr(settings_mod, "load_options_switch_visible", lambda: True)
    if enabled is not None:
        monkeypatch.setattr(settings_mod, "load_circuit_enabled", lambda: enabled)
    from gui.widgets.main_window import MainWindow

    return MainWindow(), made


def _close_window(window, app) -> None:
    window._bridge = None
    window.close()
    window.deleteLater()
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    app.processEvents()


def test_switched_off_the_page_holds_its_slot_hidden_and_asks_nothing(app, monkeypatch):
    from gui.widgets import main_window as mw
    from gui.widgets.circuit import CircuitWidget

    window, made = _main_window(monkeypatch, enabled=False)
    try:
        page = window._unwrap(window._stacked.widget(mw._PAGE_CIRCUIT))
        assert isinstance(page, CircuitWidget)
        assert not page.is_enabled()
        assert not window._sidebar.is_page_visible(mw._PAGE_CIRCUIT)
        assert window._stacked.widget(mw._PAGE_SETTINGS) is window._settings_widget
        window._switch_page(mw._PAGE_CIRCUIT)  # a hidden page is refused
        assert window._stacked.currentIndex() == mw._PAGE_DASHBOARD
        assert made == []
    finally:
        _close_window(window, app)


def test_switching_it_on_and_off_takes_effect_at_once(app, monkeypatch):
    from gui.widgets import main_window as mw

    window, made = _main_window(monkeypatch, enabled=False)
    try:
        window.show()
        page = window._unwrap(window._circuit_widget)
        settings_page = window._unwrap(window._settings_widget)
        settings_page.circuit_enabled_changed.emit(True)
        assert page.is_enabled()
        assert window._sidebar.is_page_visible(mw._PAGE_CIRCUIT)
        assert made == [], "on, but not open: nothing asked yet"

        window._switch_page(mw._PAGE_CIRCUIT)
        assert window._stacked.currentIndex() == mw._PAGE_CIRCUIT
        _wait_until(app, lambda: made == [1] and not page.fetching())

        settings_page.circuit_enabled_changed.emit(False)
        assert not page.is_enabled()
        assert not window._sidebar.is_page_visible(mw._PAGE_CIRCUIT)
        assert window._stacked.currentIndex() == mw._PAGE_DASHBOARD
    finally:
        _close_window(window, app)


def test_switched_on_at_launch_the_entry_is_shown(app, monkeypatch):
    from gui.widgets import main_window as mw

    window, made = _main_window(monkeypatch, enabled=True)
    try:
        assert window._sidebar.is_page_visible(mw._PAGE_CIRCUIT)
        assert window._unwrap(window._circuit_widget).is_enabled()
        assert made == [], "the window is built; the page is not open"
    finally:
        _close_window(window, app)


def test_by_default_the_page_is_on_and_the_window_opening_asks_nothing(
    app, monkeypatch, real_circuit_setting
):
    from gui.widgets import main_window as mw

    QSettings("XOP", "XOPTrader").remove("circuit/enabled")
    window, made = _main_window(monkeypatch, enabled=None)
    try:
        window.show()
        app.processEvents()
        assert window._sidebar.is_page_visible(mw._PAGE_CIRCUIT)
        page = window._unwrap(window._circuit_widget)
        assert page.is_enabled()
        assert window._stacked.currentIndex() == mw._PAGE_DASHBOARD
        assert made == [] and not page.fetching(), "the page is not open, so nothing is asked"
    finally:
        _close_window(window, app)
