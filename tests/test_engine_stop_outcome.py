"""[shutdown-flag-race 2026-09-12] EngineBridge._stop_engine_process reports what happened.

At 22:41:14.587 the closing GUI logged that its engine had exited gracefully --
six milliseconds after a newly launched GUI had terminated that engine, which
never read its stop request -- and after its own terminate() it used to report
a clean exit. Each test drives the real _stop_engine_process with a scripted
Popen double.

Every test captures at DEBUG. Both pre-fix lies were INFO records, and pytest
captures at WARNING unless told otherwise, so without caplog.set_level every
"never says gracefully" assertion here would pass with the lie reinstated.
"""

from __future__ import annotations

import logging
import os
import subprocess

import pytest

pytest.importorskip("PySide6")

from gui import shutdown_flag  # noqa: E402
from gui.services.engine_bridge import EngineBridge  # noqa: E402

ENGINE_PID = 15916


class ScriptedEngine:
    """A Popen double whose wait() plays *script(proc, timeout)*."""

    def __init__(self, pid, script, returncode=None):
        self.pid = pid
        self.returncode = returncode
        self._script = script
        self.waits = []
        self.terminated = False
        self.killed = False

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        self.waits.append(timeout)
        self._script(self, timeout)
        return self.returncode

    def terminate(self):
        self.terminated = True

    def kill(self):
        self.killed = True


class LogHandle:
    def __init__(self):
        self.closed = False

    def close(self):
        self.closed = True


def _bridge(tmp_path, proc):
    bridge = EngineBridge.__new__(EngineBridge)  # stop logic only, no Qt init
    bridge._db_path = tmp_path / "data" / "xop_trader.db"
    bridge._engine_process = proc
    bridge._engine_log_fh = None
    bridge._engine_log_path = None
    bridge._engine_launch_dir = None
    return bridge


def _flag(tmp_path):
    return tmp_path / "data" / shutdown_flag.FLAG_NAME


def _messages(caplog, level=None):
    return [
        record.getMessage()
        for record in caplog.records
        if level is None or record.levelno == level
    ]


def _never_says(caplog, *phrases):
    for message in _messages(caplog):
        for phrase in phrases:
            assert phrase not in message, f"{phrase!r} was logged: {message}"


def test_incident_external_kill_is_not_reported_graceful_and_leaves_no_flag(tmp_path, caplog):
    caplog.set_level(logging.DEBUG)
    seen = {}

    def another_gui_kills_it(proc, timeout):
        assert timeout == 30
        seen["request"] = _flag(tmp_path).read_bytes()
        proc.returncode = 15  # os.kill(SIGTERM) on Windows: TerminateProcess(15)

    proc = ScriptedEngine(ENGINE_PID, another_gui_kills_it)
    bridge = _bridge(tmp_path, proc)
    bridge._stop_engine_process()

    parsed = shutdown_flag.parse_shutdown_request(seen["request"].decode("ascii"))
    assert (parsed.kind, parsed.pid) == (shutdown_flag.RequestKind.ADDRESSED, ENGINE_PID)
    _never_says(caplog, "gracefully")
    assert any("NOT consumed" in m for m in _messages(caplog, logging.WARNING))
    assert not _flag(tmp_path).exists(), "an undelivered request must not wait for the next engine"
    assert not proc.terminated
    assert bridge._engine_process is None


def test_graceful_stop_consumes_an_addressed_request_and_tears_down(tmp_path, caplog):
    caplog.set_level(logging.DEBUG)
    seen = {}

    def engine_honours_it(proc, timeout):
        assert timeout == 30
        flag = _flag(tmp_path)
        seen["request"] = flag.read_bytes()
        flag.unlink()  # the engine consumes the request ...
        proc.returncode = 0  # ... cancels its book and exits cleanly

    proc = ScriptedEngine(ENGINE_PID, engine_honours_it)
    bridge = _bridge(tmp_path, proc)
    log_handle = LogHandle()
    bridge._engine_log_fh = log_handle
    outcome = bridge._stop_engine_process()

    parsed = shutdown_flag.parse_shutdown_request(seen["request"].decode("ascii"))
    assert (parsed.kind, parsed.pid) == (shutdown_flag.RequestKind.ADDRESSED, ENGINE_PID)
    assert outcome is shutdown_flag.StopOutcome.GRACEFUL
    assert any("exited gracefully" in m for m in _messages(caplog, logging.INFO))
    assert not proc.terminated
    assert not _flag(tmp_path).exists()
    assert bridge._engine_process is None, "teardown must run on the graceful path too"
    assert log_handle.closed and bridge._engine_log_fh is None


def test_unconsumed_request_is_removed_after_forced_termination(tmp_path, caplog):
    caplog.set_level(logging.DEBUG)

    def ignores_it_then_dies_on_terminate(proc, timeout):
        if timeout == 30:
            raise subprocess.TimeoutExpired("xop_trader.exe", timeout)
        assert timeout == 10 and proc.terminated
        proc.returncode = 1  # TerminateProcess(1)

    proc = ScriptedEngine(ENGINE_PID, ignores_it_then_dies_on_terminate)
    _bridge(tmp_path, proc)._stop_engine_process()

    assert proc.terminated
    _never_says(caplog, "gracefully", "exited cleanly")
    assert any("before it consumed" in m for m in _messages(caplog, logging.WARNING))
    assert not _flag(tmp_path).exists()


def test_consumed_but_unfinished_stop_is_reported_as_terminated(tmp_path, caplog):
    caplog.set_level(logging.DEBUG)

    def consumes_it_but_hangs(proc, timeout):
        if timeout == 30:
            _flag(tmp_path).unlink()  # consumed; the shutdown cancel then hangs
            raise subprocess.TimeoutExpired("xop_trader.exe", timeout)
        proc.returncode = 1

    proc = ScriptedEngine(ENGINE_PID, consumes_it_but_hangs)
    _bridge(tmp_path, proc)._stop_engine_process()

    warnings = _messages(caplog, logging.WARNING)
    assert any(
        "was terminated" in m and "had been removed by the engine" in m for m in warnings
    )
    _never_says(caplog, "gracefully")


def test_unkillable_engine_keeps_its_request(tmp_path, caplog):
    caplog.set_level(logging.DEBUG)

    def never_exits(proc, timeout):
        raise subprocess.TimeoutExpired("xop_trader.exe", timeout)

    proc = ScriptedEngine(ENGINE_PID, never_exits)
    bridge = _bridge(tmp_path, proc)
    outcome = bridge._stop_engine_process()

    assert outcome is shutdown_flag.StopOutcome.STILL_RUNNING
    assert any("still running" in m for m in _messages(caplog, logging.ERROR))
    assert proc.terminated and proc.killed
    assert shutdown_flag.flag_names_pid(_flag(tmp_path), ENGINE_PID), (
        "the engine is still alive and may yet honour its request")
    assert bridge._engine_process is proc, (
        "a live engine must stay managed -- a cleared handle lets start_engine() "
        "launch a second engine beside it")


def test_unwritable_flag_is_reported_as_termination_without_request(tmp_path, caplog, monkeypatch):
    caplog.set_level(logging.DEBUG)

    def refuse(*_args, **_kwargs):
        raise OSError(13, "the data directory is read-only")

    monkeypatch.setattr(shutdown_flag, "write_shutdown_request", refuse)

    def dies_on_terminate(proc, timeout):
        if timeout == 30:
            raise subprocess.TimeoutExpired("xop_trader.exe", timeout)
        proc.returncode = 1

    proc = ScriptedEngine(ENGINE_PID, dies_on_terminate)
    _bridge(tmp_path, proc)._stop_engine_process()

    assert proc.terminated
    assert any("without a graceful request" in m for m in _messages(caplog, logging.WARNING))
    _never_says(caplog, "gracefully")


def test_an_unaddressable_pid_ends_in_termination_not_an_exception(tmp_path, caplog):
    caplog.set_level(logging.DEBUG)

    def dies_on_terminate(proc, timeout):
        assert timeout == 10, "no request was written, so nothing is waited 30 s for"
        proc.returncode = 1

    # render_shutdown_request refuses PID 0 with ValueError. Escaping
    # bridge.shutdown() from aboutToQuit, that would skip terminate() and
    # leave the engine running unmanaged.
    proc = ScriptedEngine(0, dies_on_terminate)
    _bridge(tmp_path, proc)._stop_engine_process()

    assert proc.terminated
    assert any("without a graceful request" in m for m in _messages(caplog, logging.WARNING))


def test_an_engine_that_already_exited_is_torn_down_without_a_request(tmp_path, caplog):
    caplog.set_level(logging.DEBUG)

    def must_not_wait(_proc, _timeout):
        raise AssertionError("an exited engine is not waited for")

    proc = ScriptedEngine(ENGINE_PID, must_not_wait, returncode=3)
    bridge = _bridge(tmp_path, proc)

    assert bridge._stop_engine_process() is None
    assert any("already exited (rc=3)" in m for m in _messages(caplog, logging.INFO))
    assert not _flag(tmp_path).exists()
    assert bridge._engine_process is None


# --------------------------------------------------------------------------- #
# The stop marker a relaunched GUI waits on
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("ending", ["graceful", "terminated", "still-running"])
def test_the_stop_marker_lasts_exactly_as_long_as_the_stop(tmp_path, ending):
    """[review] The engine removes shutdown.flag as soon as it consumes it --
    before it cancels its book -- so the marker is what shows a relaunched GUI
    that this stop is still under way. It must be in place whenever the engine
    is waited for, and gone once the stop is over, however it ended."""
    marker = shutdown_flag.stop_marker_path(_flag(tmp_path))
    seen = []

    def script(proc, timeout):
        seen.append(shutdown_flag.read_shutdown_request(marker))
        if ending == "graceful":
            _flag(tmp_path).unlink()  # consumed ...
            proc.returncode = 0  # ... the book cancelled, the engine gone
        elif ending == "terminated" and timeout == 10:
            proc.returncode = 1
        else:
            raise subprocess.TimeoutExpired("xop_trader.exe", timeout)

    proc = ScriptedEngine(ENGINE_PID, script)
    _bridge(tmp_path, proc)._stop_engine_process()

    assert len(seen) == {"graceful": 1, "terminated": 2, "still-running": 3}[ending]
    for request in seen:
        assert request is not None, "the marker is in place whenever the engine is waited for"
        assert (request.kind, request.pid, request.requester_pid) == (
            shutdown_flag.RequestKind.ADDRESSED, ENGINE_PID, os.getpid())
    assert not marker.exists(), "the stop is over, whatever its outcome"


def test_a_stop_that_raises_still_removes_its_marker(tmp_path):
    """A marker naming a running engine and a running GUI would make every
    later relaunch sit out its bounded wait for a stop nobody is performing."""
    marker = shutdown_flag.stop_marker_path(_flag(tmp_path))
    seen = []

    class RefusesTerminate(ScriptedEngine):
        def terminate(self):
            raise PermissionError(5, "Access is denied")

    def times_out(proc, timeout):
        seen.append(marker.exists())
        raise subprocess.TimeoutExpired("xop_trader.exe", timeout)

    proc = RefusesTerminate(ENGINE_PID, times_out)
    with pytest.raises(PermissionError):
        _bridge(tmp_path, proc)._stop_engine_process()

    assert seen == [True], "the marker was in place while the engine was waited for"
    assert not marker.exists()
