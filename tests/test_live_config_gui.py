"""[LIVECFG] The GUI half of applying a Settings save to the running engine.

The engine answers each reload in data/config_reload.result.json (the C++
half is pinned in cpp/tests/test_config_live_apply.cpp). These tests pin
the GUI side: the answer is read once, stale answers are ignored, and the
Settings page names exactly the settings that still wait for a restart.

Runs headless (``QT_QPA_PLATFORM=offscreen``).
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path
from types import SimpleNamespace

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402

pytest.importorskip("PySide6")

from PySide6.QtWidgets import QApplication  # noqa: E402


@pytest.fixture(scope="session")
def qapp():
    app = QApplication.instance() or QApplication([])
    yield app


def _write_result(path: Path, **fields) -> None:
    payload = {"status": "applied", "error": "", "restart_required": [],
               "request_id": "req-1", "processed_at": int(time.time())}
    payload.update(fields)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload), encoding="utf-8")


# --------------------------------------------------------------------------- #
# read_reload_result
# --------------------------------------------------------------------------- #

def test_a_fresh_answer_is_read_and_normalised(tmp_path):
    from gui.services.engine_bridge import read_reload_result

    path = tmp_path / "config_reload.result.json"
    _write_result(path, restart_required=["database.path"], error=None)
    result = read_reload_result(path, request_id="req-1")
    assert result is not None
    assert result["status"] == "applied"
    assert result["error"] == ""
    assert result["restart_required"] == ["database.path"]


def test_an_answer_to_another_request_is_ignored(tmp_path):
    # Even one written in the same second: only the echoed id counts.
    from gui.services.engine_bridge import read_reload_result

    path = tmp_path / "config_reload.result.json"
    _write_result(path, request_id="req-0")
    assert read_reload_result(path, request_id="req-1") is None


@pytest.mark.parametrize("body", ["", "not json", "[1, 2]",
                                  '{"status": "applied"}'])
def test_a_missing_or_malformed_answer_is_none(tmp_path, body):
    from gui.services.engine_bridge import read_reload_result

    path = tmp_path / "config_reload.result.json"
    assert read_reload_result(path, request_id="req-1") is None
    path.write_text(body, encoding="utf-8")
    assert read_reload_result(path, request_id="req-1") is None


# --------------------------------------------------------------------------- #
# EngineBridge: one answer per request
# --------------------------------------------------------------------------- #

def _bridge(tmp_path):
    from gui.services.engine_bridge import EngineBridge

    bridge = EngineBridge.__new__(EngineBridge)
    bridge._db_path = tmp_path / "data" / "xop_trader.db"
    bridge._reload_request_id = None
    bridge._reload_deferred_reported = False
    return bridge


def _flag_id(bridge) -> str:
    flag = bridge._db_path.parent / "config_reload.flag"
    word, request_id = flag.read_text(encoding="utf-8").split()
    assert word == "reload"
    return request_id


def test_the_bridge_reports_each_answer_once(tmp_path):
    bridge = _bridge(tmp_path)
    assert bridge.read_config_reload_result() is None, "no request pending"

    bridge.request_config_reload()
    request_id = _flag_id(bridge)
    _write_result(bridge.config_reload_result_path, request_id="older")
    assert bridge.read_config_reload_result() is None, "engine has not answered"

    _write_result(bridge.config_reload_result_path, request_id=request_id,
                  restart_required=["monitoring.prometheus_port"])
    result = bridge.read_config_reload_result()
    assert result is not None
    assert result["restart_required"] == ["monitoring.prometheus_port"]
    assert bridge.read_config_reload_result() is None, "reported once"


def test_a_deferred_answer_is_followed_by_the_final_one(tmp_path):
    bridge = _bridge(tmp_path)
    bridge.request_config_reload()
    request_id = _flag_id(bridge)
    path = bridge.config_reload_result_path

    _write_result(path, request_id=request_id, status="deferred")
    assert bridge.read_config_reload_result()["status"] == "deferred"
    assert bridge.read_config_reload_result() is None, "interim reported once"

    _write_result(path, request_id=request_id, status="applied")
    assert bridge.read_config_reload_result()["status"] == "applied"
    assert bridge.read_config_reload_result() is None


# --------------------------------------------------------------------------- #
# Settings page: the restart row
# --------------------------------------------------------------------------- #

@pytest.fixture
def panel(qapp, monkeypatch):
    from gui.widgets.settings import SettingsWidget

    monkeypatch.setattr(
        SettingsWidget, "_refresh_suggested_targets", lambda self: None
    )
    w = SettingsWidget()
    yield w
    w.deleteLater()


def test_the_restart_row_names_the_waiting_settings(panel):
    panel.note_restart_required(["database.path", "pair XCH/C enabled"])
    assert not panel._restart_btn.isHidden()
    assert "database.path" in panel._restart_label.text()
    assert "pair XCH/C enabled" in panel._restart_label.text()

    assert "reopened" in panel._restart_label.text()

    panel.note_restart_required([])
    assert panel._restart_btn.isHidden()
    assert panel._restart_label.isHidden()


def _route(result: dict, saved_path=None) -> SimpleNamespace:
    from gui.widgets.main_window import MainWindow

    calls = SimpleNamespace(state=None, restart=None)
    settings = SimpleNamespace(
        note_reload_state=lambda text: setattr(calls, "state", text),
        note_restart_required=lambda items: setattr(calls, "restart", items),
    )
    window = SimpleNamespace(_settings_widget=settings, _unwrap=lambda w: w,
                             _reload_saved_path=saved_path)
    MainWindow._on_config_reload_result(window, result)
    return calls


def test_an_applied_save_with_restart_items_lists_them():
    calls = _route({"status": "applied",
                    "restart_required": ["chia wallet host/port"]})
    assert "except" in calls.state
    assert calls.restart == ["chia wallet host/port"]


def test_a_fully_live_save_clears_the_restart_row():
    calls = _route({"status": "applied", "restart_required": []})
    assert calls.state == "applied live by the running engine"
    assert calls.restart == []


def test_a_rejected_save_shows_the_error_and_no_restart_list():
    calls = _route({"status": "rejected", "error": "bad tiers",
                    "restart_required": ["database.path"]})
    assert "REJECTED" in calls.state and "bad tiers" in calls.state
    assert calls.restart == []


def test_an_answer_about_another_config_file_is_not_this_saves(tmp_path):
    # Settings saved B.yaml; the engine beside the same database runs
    # A.yaml and answered the shared flag with A's reload.
    saved = tmp_path / "B.yaml"
    calls = _route({"status": "applied", "restart_required": [],
                    "config_file": str(tmp_path / "A.yaml")},
                   saved_path=str(saved))
    assert "DIFFERENT" in calls.state and "NOT" in calls.state
    assert calls.restart == []

    calls = _route({"status": "applied", "restart_required": [],
                    "config_file": str(saved)}, saved_path=str(saved))
    assert calls.state == "applied live by the running engine"


def test_a_failed_restart_keeps_the_restart_list():
    from gui.widgets.main_window import MainWindow

    calls = SimpleNamespace(state=None, restart="untouched")
    settings = SimpleNamespace(
        note_reload_state=lambda text: setattr(calls, "state", text),
        note_restart_required=lambda items: setattr(calls, "restart", items),
    )
    bridge = SimpleNamespace(engine_running_locally=True)

    def _stop(**_kw):
        bridge.engine_running_locally = False

    bridge.stop_engine = _stop
    bridge.start_engine = lambda: None   # launch failed: still not running
    window = SimpleNamespace(
        _bridge=bridge, _settings_widget=settings, _unwrap=lambda w: w,
        _engine_stop_is_ours=lambda: True,
        _decide_engine_stop=lambda closing: SimpleNamespace(
            proceed=True, policy="keep"),
        _style_start_stop_button=lambda: None,
        _style_pause_resume_button=lambda: None,
    )
    MainWindow._on_restart_engine_for_settings(window)
    assert "did NOT start" in calls.state
    assert calls.restart == "untouched"


def test_a_restart_that_fails_after_launch_restores_the_list():
    # Popen succeeded, so the row was cleared; the bridge's early-exit
    # check then reports the engine died during startup.
    from gui.widgets.main_window import MainWindow

    calls = SimpleNamespace(state=None, restart=None)
    settings = SimpleNamespace(
        note_reload_state=lambda text: setattr(calls, "state", text),
        note_restart_required=lambda items: setattr(calls, "restart", items),
        restart_required_items=lambda: ["chia wallet host/port"],
    )
    bridge = SimpleNamespace(engine_running_locally=True)
    bridge.stop_engine = lambda **_kw: setattr(
        bridge, "engine_running_locally", False)
    bridge.start_engine = lambda: setattr(
        bridge, "engine_running_locally", True)
    window = SimpleNamespace(
        _bridge=bridge, _settings_widget=settings, _unwrap=lambda w: w,
        _engine_stop_is_ours=lambda: True,
        _decide_engine_stop=lambda closing: SimpleNamespace(
            proceed=True, policy="keep"),
        _style_start_stop_button=lambda: None,
        _style_pause_resume_button=lambda: None,
        _RESTART_FAILURE_WINDOW_S=MainWindow._RESTART_FAILURE_WINDOW_S,
    )
    MainWindow._on_restart_engine_for_settings(window)
    assert calls.restart == []
    assert calls.state == "engine restarted on the saved config"

    MainWindow._restore_restart_items_after_failed_start(window)
    assert calls.restart == ["chia wallet host/port"]
    assert "did NOT start" in calls.state

    calls.restart = None
    MainWindow._restore_restart_items_after_failed_start(window)
    assert calls.restart is None, "restored once, for that restart only"


def test_the_settings_page_reports_its_restart_list(panel):
    panel.note_restart_required(["monitoring.prometheus_port"])
    assert panel.restart_required_items() == ["monitoring.prometheus_port"]
    panel.note_restart_required([])
    assert panel.restart_required_items() == []
