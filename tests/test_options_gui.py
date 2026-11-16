"""[OPTIONS] The Options page, its switch, and the engine interlock.

* The page drives an ``OptionsRunner`` (a double here; the runner itself is
  gui/services/options/tests/test_runner.py): Off / Rehearse / Live, Live only
  after a confirmation, a refusal shown and the switch put back, the mode
  remembered and taken up again at the next launch without asking.
* ``EngineBridge`` will not start the engine, at launch or on any button, while
  an options book sharing the trading wallet has contracts open. On 2026-11-15
  the launch-time auto-start did exactly that and the engine spent the book's
  coins within minutes.
* Start Trading does not show a running engine when the start was refused.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from types import SimpleNamespace

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402

pytest.importorskip("PySide6")
yaml = pytest.importorskip("yaml")

from PySide6.QtCore import QCoreApplication, QEvent, QSettings  # noqa: E402
from PySide6.QtWidgets import QApplication, QMessageBox  # noqa: E402

from gui.services.options import runner as options_runner  # noqa: E402
from gui.services.options import store  # noqa: E402
from gui.widgets import options as options_page  # noqa: E402

Mode = options_runner.Mode
USD = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"


@pytest.fixture(scope="module")
def app():
    instance = QApplication.instance() or QApplication(sys.argv)
    yield instance


@pytest.fixture(autouse=True)
def own_settings(monkeypatch):
    """The page's QSettings in a store of the tests' own, emptied before and after."""
    monkeypatch.setattr(options_page, "_SETTINGS_ORG", "XOP-tests-options")
    QSettings("XOP-tests-options", options_page._SETTINGS_APP).clear()
    yield
    QSettings("XOP-tests-options", options_page._SETTINGS_APP).clear()


def _config(folder: Path, *, options: bool = True, shared: bool = False) -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    wallet = {"port": 9356, "cert_path": "c", "key_path": "k"}
    chia = {"wallet_host": "localhost", "wallet_port": 9256}
    if shared:
        wallet = {
            "port": 9256,
            "cert_path": "c",
            "key_path": "k",
            "fingerprint": 42,
            "share_trading_wallet": True,
            "address": "xch1kxxd76v9kwsxrcxgndn3mylnxjhd7klgdns7083zjl247ffge32sr5rsy2",
        }
        chia["wallet_fingerprint"] = 42
    root: dict = {"chia": chia}
    if options:
        root["options"] = {
            "enabled": True,
            "wallet": wallet,
            "quote_asset_id": USD,
            "contract_xch": 0.5,
        }
    path = folder / "config.yaml"
    path.write_text(yaml.safe_dump(root), encoding="utf-8")
    return path


def _open_contract(config_path: Path) -> None:
    db = config_path.parent / "data" / "options_jobs.db"
    db.parent.mkdir(parents=True, exist_ok=True)
    book = store.OptionStore(str(db))
    book.add_contract(
        launcher_id=b"\x01" * 32,
        slot="C110@1",
        side="call",
        underlying_asset=None,
        underlying_amount=500_000_000_000,
        strike_asset=bytes.fromhex(USD),
        strike_amount=625,
        expiration=2_000_000_000,
        creator_puzzle_hash=b"\x02" * 32,
        bundle=b"",
        deadline=1,
        now=0,
    )
    book.close()


class RunnerDouble:
    def __init__(self, refuse: str = "") -> None:
        self.mode = Mode.OFF
        self.starts: list[Mode] = []
        self.stops = 0
        self.refuse = refuse
        self.snap = options_runner.Snapshot()

    def start(self, mode: Mode) -> None:
        if self.refuse:
            raise options_runner.RunnerError(self.refuse)
        self.starts.append(mode)
        self.mode = mode
        self.snap = options_runner.Snapshot(mode=mode, running=True)

    def stop(self) -> None:
        self.stops += 1
        self.mode = Mode.OFF
        self.snap = options_runner.Snapshot()

    def snapshot(self) -> options_runner.Snapshot:
        return self.snap


def _answer(monkeypatch, reply):
    asked: list[str] = []

    def question(parent, title, text, *args, **kwargs):
        asked.append(title)
        return reply

    monkeypatch.setattr(QMessageBox, "question", staticmethod(question))
    return asked


def _page(app, tmp_path, runner, **attach):
    page = options_page.OptionsWidget(runner=runner)
    page.attach(_config(tmp_path, **attach.pop("config", {})), **attach)
    return page


def _close(page, app) -> None:
    page.stop_background_work()
    page.deleteLater()
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    app.processEvents()


# --------------------------------------------------------------------------- #
# The page
# --------------------------------------------------------------------------- #


def test_options_not_set_up_leave_the_switch_off_and_say_why(app, tmp_path):
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner, config={"options": False})
    try:
        assert not page._buttons[Mode.REHEARSE].isEnabled()
        assert not page._buttons[Mode.LIVE].isEnabled()
        assert "options.enabled is not true" in page._setup.text()
        assert runner.starts == []
    finally:
        _close(page, app)


def test_live_asks_first_and_no_means_no(app, tmp_path, monkeypatch):
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner)
    try:
        asked = _answer(monkeypatch, QMessageBox.StandardButton.No)
        page._buttons[Mode.LIVE].click()
        assert asked == ["Options: go Live"] and runner.starts == []
        assert page._buttons[Mode.OFF].isChecked()

        _answer(monkeypatch, QMessageBox.StandardButton.Yes)
        page._buttons[Mode.LIVE].click()
        assert runner.starts == [Mode.LIVE]
        assert (
            QSettings("XOP-tests-options", options_page._SETTINGS_APP).value(options_page.KEY_MODE)
            == "live"
        )
    finally:
        _close(page, app)


def test_rehearse_needs_no_confirmation_and_off_stops(app, tmp_path, monkeypatch):
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner)
    try:
        asked = _answer(monkeypatch, QMessageBox.StandardButton.No)
        page._buttons[Mode.REHEARSE].click()
        page._buttons[Mode.OFF].click()
        assert asked == [] and runner.starts == [Mode.REHEARSE] and runner.stops == 1
        assert (
            QSettings("XOP-tests-options", options_page._SETTINGS_APP).value(options_page.KEY_MODE)
            == "off"
        )
    finally:
        _close(page, app)


def test_a_refused_live_says_why_and_the_switch_goes_back(app, tmp_path, monkeypatch):
    runner = RunnerDouble(refuse="Live refused: the trading engine is RUNNING")
    page = _page(app, tmp_path, runner)
    try:
        _answer(monkeypatch, QMessageBox.StandardButton.Yes)
        page._buttons[Mode.LIVE].click()
        assert "Live refused" in page._held.text()
        assert page._buttons[Mode.OFF].isChecked()
        assert (
            QSettings("XOP-tests-options", options_page._SETTINGS_APP).value(options_page.KEY_MODE)
            is None
        )  # a refusal is not remembered
    finally:
        _close(page, app)


def test_the_remembered_mode_is_taken_up_at_launch_without_asking(app, tmp_path, monkeypatch):
    QSettings("XOP-tests-options", options_page._SETTINGS_APP).setValue(
        options_page.KEY_MODE, "rehearse"
    )
    asked = _answer(monkeypatch, QMessageBox.StandardButton.No)
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner)
    try:
        assert runner.starts == [Mode.REHEARSE] and asked == []
        assert page._buttons[Mode.REHEARSE].isChecked()
    finally:
        _close(page, app)


def test_recent_activity_keeps_coming_once_the_runner_s_history_is_full(app, tmp_path):
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner)
    try:
        runner.snap = options_runner.Snapshot(recent=("a", "b", "c"), recent_count=3)
        page.refresh()
        # Full: the oldest dropped, one new, the length unchanged.
        runner.snap = options_runner.Snapshot(recent=("b", "c", "d"), recent_count=4)
        page.refresh()
        assert page._recent.toPlainText().splitlines() == ["a", "b", "c", "d"]
    finally:
        _close(page, app)


def test_closing_the_app_stops_the_book(app, tmp_path):
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner)
    page.stop_background_work()
    assert runner.stops == 1
    _close(page, app)


# --------------------------------------------------------------------------- #
# The engine interlock
# --------------------------------------------------------------------------- #


def _bridge(app, config_path: Path):
    from gui.services.engine_bridge import EngineBridge

    bridge = EngineBridge(config_path=config_path)
    errors: list[str] = []
    bridge.error.connect(errors.append)
    return bridge, errors


def test_start_engine_is_refused_while_a_shared_book_has_contracts_open(app, tmp_path, monkeypatch):
    config_path = _config(tmp_path, shared=True)
    bridge, errors = _bridge(app, config_path)
    try:
        launched: list[int] = []
        monkeypatch.setattr(bridge, "_start_engine_process", lambda: launched.append(1) or True)
        monkeypatch.setattr(bridge, "_publish_retained_warp_store", lambda: None)

        bridge.start_engine()  # nothing open yet: it starts
        assert launched == [1]

        _open_contract(config_path)
        monkeypatch.setattr(bridge, "_engine_process", None)
        bridge.start_engine()
        assert launched == [1], "no second launch over an open contract"
        assert errors and "Engine not started" in errors[-1] and "contract(s) open" in errors[-1]
    finally:
        bridge.deleteLater()
        app.processEvents()


def test_the_launch_time_auto_start_is_held_too(app, tmp_path, monkeypatch):
    from gui.services import engine_bridge as eb

    config_path = _config(tmp_path, shared=True)
    _open_contract(config_path)
    bridge, errors = _bridge(app, config_path)
    try:
        launched: list[int] = []
        monkeypatch.setattr(bridge, "_is_engine_reachable", lambda: False)
        monkeypatch.setattr(bridge, "_start_engine_process", lambda: launched.append(1) or True)
        monkeypatch.setattr(eb, "QTimer", SimpleNamespace(singleShot=lambda ms, fn: fn()))

        bridge._auto_start_engine()

        assert launched == []
        assert errors and "Engine not started" in errors[-1]
    finally:
        bridge.deleteLater()
        app.processEvents()


def test_options_on_their_own_wallet_never_hold_the_engine(app, tmp_path, monkeypatch):
    config_path = _config(tmp_path, shared=False)
    _open_contract(config_path)
    bridge, errors = _bridge(app, config_path)
    try:
        assert bridge._options_blocker() is None
    finally:
        bridge.deleteLater()
        app.processEvents()


def test_a_failing_check_holds_the_engine(app, tmp_path, monkeypatch):
    bridge, errors = _bridge(app, _config(tmp_path, shared=True))
    try:

        def broken(path):
            raise RuntimeError("database is locked")

        monkeypatch.setattr(options_runner, "engine_start_blocker", broken)
        why = bridge._options_blocker()
        assert why is not None and "could not be checked" in why
    finally:
        bridge.deleteLater()
        app.processEvents()


# --------------------------------------------------------------------------- #
# Start Trading after a refused start
# --------------------------------------------------------------------------- #


class _RefusingBridge:
    engine_running_locally = False
    engine_supports_keep_offers = True
    stop_offers_default = "cancel"

    def __init__(self) -> None:
        self.starts = 0

    def start_engine(self) -> None:
        self.starts += 1  # refused: nothing launched, engine_running_locally stays False


def test_start_trading_does_not_show_a_running_engine_that_was_refused(app, monkeypatch):
    from gui.widgets.main_window import MainWindow

    window = MainWindow()
    try:
        _answer(monkeypatch, QMessageBox.StandardButton.Yes)
        bridge = _RefusingBridge()
        window._bridge = bridge
        window._bot_running = False

        window._on_start_stop()

        assert bridge.starts == 1
        assert window._bot_running is False
    finally:
        window._bridge = None
        window.close()
        window.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        app.processEvents()
