"""[shutdown-flag-race 2026-09-12] The GUI singleton closes the INSTALLED GUI,
lets a stop that is already under way finish, and cleans up after itself.

At 23:24:21 a relaunched installed GUI logged "No old GUI or engine instances
found" while the 22:41 GUI was still running: the kill list knew only
xoptrader-gui.exe and xoptrader_gui.exe, never xop_trader_gui.exe. Fixing only
the name would terminate a GUI that is inside its blocking stop, so a stop
already under way -- shutdown.flag, or the closing GUI's stop marker once the
engine has consumed the flag, naming a running engine -- is waited for
(bounded, 45 s) first, and a request whose engine is terminated anyway is
removed afterwards.

os.kill, subprocess.run, time.sleep and the process-exit wait are patched
before every call that could reach them: on Windows os.kill IS
TerminateProcess, and these tests must never touch a real process. gui/main.py
imports time at module level, so time.sleep is patched on the global module.
The one exception is the last test, which observes a child it started itself.
"""

from __future__ import annotations

import json
import logging
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest
import yaml

pytest.importorskip("PySide6")

from gui import main as gui_main  # noqa: E402
from gui import shutdown_flag  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
OWN_PIDS = {100, 99}  # this GUI and its launcher -- protected
ENGINE = 5001
BOOTLOADER, CHILD = 4001, 4002  # one installed GUI is two processes


def installed_gui_name() -> str:
    """Derived from the release build rather than restated."""
    release = (REPO / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
    names = set(re.findall(r"--name'?\s*,?\s*'?([A-Za-z0-9_\-]+)", release))
    assert names == {"xop_trader_gui"}, names
    return names.pop()


class FakeWindows:
    """The process table as the Windows scans see it, plus a scripted wait."""

    def __init__(self, processes):
        self.processes = dict(processes)  # pid -> ProcessName (no .exe)
        self.kills = []
        self.waits = []
        self.exits_during_wait = {}  # waited-for pid -> pids that vanish

    def run(self, cmd, **_kwargs):
        script = cmd[-1]
        if "Get-CimInstance" in script:
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
        if "Get-Process" in script:
            rows = [{"Id": pid, "ProcessName": name} for pid, name in self.processes.items()]
            return subprocess.CompletedProcess(cmd, 0, stdout=json.dumps(rows), stderr="")
        raise AssertionError(f"unexpected command: {cmd}")

    def kill(self, pid, sig):
        self.kills.append((pid, sig))
        self.processes.pop(pid, None)

    def wait(self, pid, timeout_s):
        self.waits.append((pid, timeout_s))
        vanished = self.exits_during_wait.get(pid)
        if vanished is None:
            return False  # still running when the wait ends
        for gone in vanished:
            self.processes.pop(gone, None)
        return True

    def killed_pids(self):
        return {pid for pid, _sig in self.kills}


@pytest.fixture
def windows(monkeypatch, caplog):
    caplog.set_level(logging.DEBUG)
    name = installed_gui_name()
    fake = FakeWindows({BOOTLOADER: name, CHILD: name, ENGINE: "xop_trader", 100: name})
    monkeypatch.setattr(gui_main.os, "kill", fake.kill)  # first: never a real kill
    monkeypatch.setattr(gui_main.subprocess, "run", fake.run)
    monkeypatch.setattr(time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(gui_main, "_wait_for_process_exit", fake.wait)
    return fake


def _request(path: Path, target: int, requester: int = 19084) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(shutdown_flag.render_shutdown_request(
        target, requester_pid=requester, written_at="2026-09-12T22:41:07",
    ).encode("ascii"))
    return path


def _messages(caplog, level=None):
    return [
        record.getMessage()
        for record in caplog.records
        if level is None or record.levelno == level
    ]


# --------------------------------------------------------------------------- #
# The installed name, and the count
# --------------------------------------------------------------------------- #

def test_installed_gui_executable_is_closed(windows):
    gui_main._kill_old_instances_win32(OWN_PIDS, None)

    assert windows.kills
    assert all(sig == signal.SIGTERM for _pid, sig in windows.kills)
    assert windows.killed_pids() == {BOOTLOADER, CHILD, ENGINE}
    assert 100 not in windows.killed_pids()
    assert windows.waits == [], "no request exists, so nothing is waited for"


def test_every_killed_instance_is_counted(windows, caplog):
    gui_main._kill_old_instances_win32(OWN_PIDS, None)

    assert any(
        "Terminated 3 old instance(s) (2 GUI process(es), 1 engine(s))" in m
        for m in _messages(caplog, logging.INFO)
    )


def test_request_for_a_killed_engine_is_cleaned_up(windows, tmp_path, caplog):
    flag = _request(tmp_path / "data" / "shutdown.flag", ENGINE)

    gui_main._kill_old_instances_win32(OWN_PIDS, flag)

    assert windows.waits == [(ENGINE, 45.0)], "the bounded wait comes before any kill"
    assert ENGINE in windows.killed_pids()
    assert not flag.exists()
    assert any(
        "5001" in m and "removed shutdown.flag" in m
        for m in _messages(caplog, logging.WARNING)
    )


@pytest.mark.parametrize(
    ("cmdline", "label"),
    [
        ("/opt/xoptrader/xop_trader_gui", "GUI"),
        ("/usr/bin/xoptrader-gui", "GUI"),
        ("python -m gui.main", "GUI"),
        ("/opt/xoptrader/xop_trader --config c.yaml", "engine"),
        ("/usr/bin/bash", None),
    ],
    ids=["installed-gui", "legacy-bundled-gui", "source-gui", "engine", "unrelated"],
)
def test_posix_instance_label(cmdline, label):
    assert gui_main._posix_instance_label(cmdline) == label


# --------------------------------------------------------------------------- #
# A stop that is already under way (operator decision 2026-09-13)
# --------------------------------------------------------------------------- #

def test_relaunch_lets_a_stop_already_under_way_finish(windows, tmp_path, caplog):
    """The operator closed the GUI -- its window vanished while it waited for
    its engine -- and relaunched."""
    flag = _request(tmp_path / "data" / "shutdown.flag", ENGINE, requester=CHILD)
    windows.exits_during_wait = {
        ENGINE: [ENGINE],  # the engine honours its request and exits
        CHILD: [CHILD, BOOTLOADER],  # then the closing GUI finishes, bootloader too
    }

    gui_main._kill_old_instances_win32(OWN_PIDS, flag)

    assert [pid for pid, _timeout in windows.waits] == [ENGINE, CHILD]
    assert windows.waits[0][1] == 45.0
    assert 0.0 <= windows.waits[1][1] <= 45.0, "one budget covers both waits"
    assert windows.killed_pids() == set(), "nothing that finished its own stop is terminated"
    info = _messages(caplog, logging.INFO)
    assert any("engine PID 5001 exited" in m for m in info)
    assert any("previous GUI PID 4002 exited" in m for m in info)


def test_relaunch_terminates_what_is_still_running_after_the_bounded_wait(
        windows, tmp_path, caplog):
    flag = _request(tmp_path / "data" / "shutdown.flag", ENGINE, requester=CHILD)

    gui_main._kill_old_instances_win32(OWN_PIDS, flag)  # nothing exits in time

    assert [pid for pid, _timeout in windows.waits] == [ENGINE, CHILD]
    assert windows.killed_pids() == {BOOTLOADER, CHILD, ENGINE}
    assert not flag.exists(), "after a forced termination the request is removed"
    assert any(
        "engine PID 5001 was still running after 45 s" in m
        for m in _messages(caplog, logging.WARNING)
    )


def test_no_wait_when_the_request_names_no_running_engine(windows, tmp_path):
    flag = _request(tmp_path / "data" / "shutdown.flag", 7777)

    gui_main._kill_old_instances_win32(OWN_PIDS, flag)

    assert windows.waits == []
    assert windows.killed_pids() == {BOOTLOADER, CHILD, ENGINE}
    assert flag.exists(), "a request for an engine this startup did not terminate is left alone"


def test_no_wait_for_an_unaddressed_request(windows, tmp_path):
    flag = tmp_path / "data" / "shutdown.flag"
    flag.parent.mkdir(parents=True)
    flag.write_bytes(b"shutdown")

    gui_main._kill_old_instances_win32(OWN_PIDS, flag)

    assert windows.waits == [], "an unaddressed request names no engine to wait for"
    assert ENGINE in windows.killed_pids()
    assert not flag.exists()


def test_relaunch_waits_for_a_stop_whose_flag_the_engine_already_consumed(
        windows, tmp_path, caplog):
    """[review] A healthy engine consumes shutdown.flag within one 5 s poll and
    only then cancels its book, while the closing GUI waits on it. A relaunch
    in that window finds no flag; the closing GUI's stop marker is what shows
    the stop is still under way."""
    flag = tmp_path / "data" / "shutdown.flag"
    _request(shutdown_flag.stop_marker_path(flag), ENGINE, requester=CHILD)
    windows.exits_during_wait = {
        ENGINE: [ENGINE],  # the engine finishes its shutdown cancel and exits
        CHILD: [CHILD, BOOTLOADER],  # then the closing GUI finishes its stop
    }
    assert not flag.exists(), "the engine has already consumed its request"

    gui_main._kill_old_instances_win32(OWN_PIDS, flag)

    assert [pid for pid, _timeout in windows.waits] == [ENGINE, CHILD]
    assert windows.waits[0][1] == 45.0
    assert windows.killed_pids() == set(), "a stop in mid-cancel is not cut off"
    assert any(
        shutdown_flag.STOP_MARKER_NAME in m and "engine PID 5001" in m
        for m in _messages(caplog, logging.WARNING)
    )


def test_a_stop_marker_whose_gui_has_gone_does_not_delay_startup(windows, tmp_path):
    """A GUI killed mid-stop never removes its marker. Once that GUI is gone
    nobody is stopping the engine, so the relaunch must not wait on it."""
    flag = tmp_path / "data" / "shutdown.flag"
    _request(shutdown_flag.stop_marker_path(flag), ENGINE, requester=7777)

    gui_main._kill_old_instances_win32(OWN_PIDS, flag)

    assert windows.waits == []
    assert windows.killed_pids() == {BOOTLOADER, CHILD, ENGINE}


# --------------------------------------------------------------------------- #
# Wiring: dry run, and the entry point's arguments
# --------------------------------------------------------------------------- #

def test_dry_run_does_not_close_the_installed_gui(windows, caplog):
    gui_main._kill_old_instances_win32(OWN_PIDS, None, dry_run=True)

    assert windows.killed_pids() == {ENGINE}
    assert any("--dry-run" in m for m in _messages(caplog, logging.WARNING))


def test_kill_old_instances_forwards_the_flag_and_dry_run_on_windows(
        windows, tmp_path, monkeypatch):
    monkeypatch.setattr(gui_main.platform, "system", lambda: "Windows")
    monkeypatch.setattr(gui_main.os, "getpid", lambda: 100)
    monkeypatch.setattr(gui_main.os, "getppid", lambda: 99)
    flag = _request(tmp_path / "data" / "shutdown.flag", ENGINE)

    gui_main._kill_old_instances(flag, dry_run=True)

    assert windows.waits == [(ENGINE, 45.0)], "the flag path reached the wait"
    assert not flag.exists(), "the flag path reached the cleanup"
    assert windows.killed_pids() == {ENGINE}, "dry_run reached the kill list"


def test_startup_resolves_the_flag_behind_the_bridges_validation_gate(tmp_path):
    home = tmp_path / "home"
    home.mkdir()
    config = home / "config.yaml"

    # A config ConfigService rejects never moves EngineBridge's database, so
    # its database.path must not move the flag either.
    config.write_text("database:\n  path: store/xop.db\n", encoding="utf-8")
    assert gui_main._resolve_startup_stop_flag(config, None) == (
        (home / "data").resolve() / "shutdown.flag")

    # A config it accepts does.
    valid = yaml.safe_load((REPO / "config.example.yaml").read_text(encoding="utf-8"))
    valid.setdefault("database", {})["path"] = "store/xop.db"
    config.write_text(yaml.safe_dump(valid), encoding="utf-8")
    assert gui_main._resolve_startup_stop_flag(config, None) == (
        (home / "store").resolve() / "shutdown.flag")


# --------------------------------------------------------------------------- #
# POSIX: the kill loop itself, not only its label helper
# --------------------------------------------------------------------------- #

def test_posix_kill_path_uses_the_label_rule_and_cleans_up(tmp_path, monkeypatch, caplog):
    caplog.set_level(logging.DEBUG)
    proc_root = tmp_path / "proc"
    for pid, argv in {
        BOOTLOADER: ["/opt/xoptrader/xop_trader_gui"],
        ENGINE: ["/opt/xoptrader/xop_trader", "--config", "c.yaml"],
        100: ["python", "-m", "gui.main"],  # this GUI: protected
        777: ["/usr/bin/bash"],
    }.items():
        (proc_root / str(pid)).mkdir(parents=True)
        (proc_root / str(pid) / "cmdline").write_text(chr(0).join(argv) + chr(0))

    signals = []
    waits = []

    def kill(pid, sig):
        signals.append((pid, sig))
        if pid == ENGINE:
            # The engine exits on SIGTERM; the GUI is stubborn and is still
            # there for the SIGKILL sweep.
            shutil.rmtree(proc_root / str(pid))

    monkeypatch.setattr(gui_main.os, "kill", kill)
    monkeypatch.setattr(time, "sleep", lambda _seconds: None)

    def still_running(pid, timeout_s):
        waits.append((pid, timeout_s))
        return False

    monkeypatch.setattr(gui_main, "_wait_for_process_exit", still_running)
    flag = _request(tmp_path / "data" / "shutdown.flag", ENGINE)

    gui_main._kill_old_instances_posix(OWN_PIDS, flag, proc_root=proc_root)

    assert waits == [(ENGINE, 45.0)]
    assert signals[:2] == [(BOOTLOADER, signal.SIGTERM), (ENGINE, signal.SIGTERM)]
    assert {pid for pid, _sig in signals} == {BOOTLOADER, ENGINE}, (
        "bash and this GUI are never signalled")
    info = _messages(caplog, logging.INFO)
    assert "[Startup] Sent SIGTERM to old GUI (PID 4001)" in info
    assert "[Startup] Sent SIGTERM to old engine (PID 5001)" in info
    assert "[Startup] Sent SIGKILL to PID 4001" in info, "the stubborn GUI is swept"
    assert "[Startup] Sent SIGKILL to PID 5001" not in info, "the engine had already exited"
    assert not flag.exists()
    assert any(
        "5001" in m and "removed shutdown.flag" in m
        for m in _messages(caplog, logging.WARNING)
    )


# --------------------------------------------------------------------------- #
# POSIX liveness: an unreaped zombie
# --------------------------------------------------------------------------- #

def test_posix_liveness_treats_an_unreaped_zombie_as_exited(tmp_path, monkeypatch):
    """A zombie still answers signal 0, so only its /proc state shows it has
    exited. The real-process test below cannot reach this branch on Linux:
    child.wait() reaps the child before the second call, so signal 0 already
    fails with ProcessLookupError."""
    signals = []
    monkeypatch.setattr(gui_main.os, "kill", lambda pid, sig: signals.append((pid, sig)))
    proc_root = tmp_path / "proc"
    stat = proc_root / "4242" / "stat"
    stat.parent.mkdir(parents=True)

    stat.write_text("4242 (xop_trader) Z 1 4242 4242 0 -1\n")
    assert gui_main._posix_process_alive(4242, proc_root=proc_root) is False

    # A command name may itself contain ")": the state follows the LAST one.
    stat.write_text("4242 (xop (trader)) S 1 4242 4242 0 -1\n")
    assert gui_main._posix_process_alive(4242, proc_root=proc_root) is True

    assert signals == [(4242, 0), (4242, 0)], "existence is probed with signal 0 only"


# --------------------------------------------------------------------------- #
# The real wait, against a real child process
# --------------------------------------------------------------------------- #

def test_wait_for_process_exit_observes_a_real_process_without_signalling_it():
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    try:
        assert gui_main._wait_for_process_exit(child.pid, 0.3) is False
        assert child.poll() is None, (
            "observing a process must never terminate it -- os.kill(pid, 0) does, "
            "on Windows")
    finally:
        child.kill()
        child.wait(timeout=30)
    assert gui_main._wait_for_process_exit(child.pid, 5.0) is True
