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
import sqlite3
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
def no_books_from_other_tests():
    """The engine check remembers every book this process ran; start each test without any."""
    with options_runner._BOOKS_LOCK:
        options_runner._BOOKS.clear()
    yield
    with options_runner._BOOKS_LOCK:
        options_runner._BOOKS.clear()


@pytest.fixture(autouse=True)
def own_settings(monkeypatch):
    """The page's QSettings in a store of the tests' own, emptied before and after."""
    monkeypatch.setattr(options_page, "_SETTINGS_ORG", "XOP-tests-options")
    QSettings("XOP-tests-options", options_page._SETTINGS_APP).clear()
    yield
    QSettings("XOP-tests-options", options_page._SETTINGS_APP).clear()


def _config(
    folder: Path, *, options: bool = True, shared: bool = False, beside: bool = False
) -> Path:
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
            "engine_fence": False,  # the unfenced shared mode; the fence is the default now
            "address": "xch1kxxd76v9kwsxrcxgndn3mylnxjhd7klgdns7083zjl247ffge32sr5rsy2",
        }
        chia["wallet_fingerprint"] = 42
    if beside:
        wallet.update(engine_beside_open_contracts=True, fund_from_wallet=True)
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

    def stop(self, timeout: float | None = None) -> None:
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


class _SyncingRunner(RunnerDouble):
    """[AUTO-FENCE] Counts the page's checks of the engine's fence flag."""

    def __init__(self, refuse: str = "") -> None:
        super().__init__(refuse)
        self.syncs = 0

    def sync_fence(self) -> None:
        self.syncs += 1


def test_attach_checks_the_fence_flag_unless_a_run_started(app, tmp_path):
    """A flag the last session left must follow the book's record, not linger."""
    runner = _SyncingRunner()
    page = _page(app, tmp_path / "a", runner)
    try:
        assert runner.syncs == 1 and runner.starts == []
    finally:
        _close(page, app)

    QSettings("XOP-tests-options", options_page._SETTINGS_APP).setValue(
        options_page.KEY_MODE, "rehearse"
    )
    resumed = _SyncingRunner()
    page = _page(app, tmp_path / "b", resumed)
    try:
        assert resumed.starts == [Mode.REHEARSE] and resumed.syncs == 0, "starting raised it"
    finally:
        _close(page, app)

    refused = _SyncingRunner(refuse="Live refused: the trading engine is RUNNING")
    QSettings("XOP-tests-options", options_page._SETTINGS_APP).setValue(
        options_page.KEY_MODE, "live"
    )
    page = _page(app, tmp_path / "c", refused)
    try:
        assert refused.syncs == 1, "a resume that could not happen still checks the flag"
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


def test_a_new_config_from_settings_stops_the_old_book_and_follows_the_new_file(
    app, tmp_path, monkeypatch
):
    class Bound(RunnerDouble):
        def __init__(self, path):
            super().__init__()
            self.config_path = path

    first = _config(tmp_path / "a")
    old = Bound(first)
    page = options_page.OptionsWidget(runner=old)
    try:
        page.attach(first)
        _answer(monkeypatch, QMessageBox.StandardButton.Yes)
        page._buttons[Mode.LIVE].click()
        assert old.starts == [Mode.LIVE]

        second = _config(tmp_path / "b")
        page.attach(second, resume=False)

        assert old.stops == 1, "the book on the old config was stopped first"
        assert page._runner is not old and page._config_path == second
        assert "switched off" in page._held.text()
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


def _settle_mints(config_path: Path) -> None:
    """Every mint the book sent has confirmed: its contracts sit open."""
    book = store.OptionStore(str(config_path.parent / "data" / "options_jobs.db"))
    try:
        for row in book.unresolved():
            book.settle_broadcast(row.id, "confirmed", contract_status=store.OPEN)
    finally:
        book.close()


def test_start_engine_runs_beside_open_contracts_when_the_config_says_so(
    app, tmp_path, monkeypatch
):
    config_path = _config(tmp_path, shared=True, beside=True)
    _open_contract(config_path)
    bridge, errors = _bridge(app, config_path)
    try:
        launched: list[int] = []
        monkeypatch.setattr(bridge, "_start_engine_process", lambda: launched.append(1) or True)
        monkeypatch.setattr(bridge, "_publish_retained_warp_store", lambda: None)

        bridge.start_engine()  # the mint is still in flight: its coins are not settled
        assert launched == []
        assert errors and "waiting to confirm (mint)" in errors[-1]

        _settle_mints(config_path)
        bridge.start_engine()  # the contract sits open, and that no longer holds it
        assert launched == [1]
        assert options_runner.engine_start_blocker(config_path) is None
    finally:
        bridge.deleteLater()
        app.processEvents()


@pytest.mark.parametrize(
    ("beside", "said"),
    [
        (False, "stays stopped while contracts are open"),
        (True, "the engine can run beside the open contracts"),
    ],
)
def test_the_live_confirmation_says_what_it_means_for_the_engine(
    app, tmp_path, monkeypatch, beside, said
):
    texts: list[str] = []

    def question(parent, title, text, *args, **kwargs):
        texts.append(text)
        return QMessageBox.StandardButton.No

    monkeypatch.setattr(QMessageBox, "question", staticmethod(question))
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner, config={"shared": True, "beside": beside})
    try:
        page._buttons[Mode.LIVE].click()
        assert len(texts) == 1 and said in texts[0]
        assert runner.starts == []
    finally:
        _close(page, app)


def test_start_engine_waits_while_a_step_holds_the_book(app, tmp_path, monkeypatch):
    # A Live step minting right now holds the acting lock: the engine must not
    # launch between "nothing is open" and that mint landing.
    config_path = _config(tmp_path, shared=True)
    db = config_path.parent / "data" / "options_jobs.db"
    db.parent.mkdir(parents=True, exist_ok=True)
    stepping = store.OptionStore(str(db))
    bridge, errors = _bridge(app, config_path)
    try:
        launched: list[int] = []
        monkeypatch.setattr(bridge, "_start_engine_process", lambda: launched.append(1) or True)
        monkeypatch.setattr(bridge, "_publish_retained_warp_store", lambda: None)
        with stepping.acting():
            bridge.start_engine()
        assert launched == [] and "in the middle of a step" in errors[-1]
        bridge.start_engine()  # the step is done: it starts
        assert launched == [1]
    finally:
        stepping.close()
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


# --------------------------------------------------------------------------- #
# The toolbar's OPTIONS switch
# --------------------------------------------------------------------------- #


def _contract(status: str = store.OPEN) -> options_runner.ContractView:
    return options_runner.ContractView(
        slot="C110@1",
        status=status,
        launcher_id="01" * 32,
        expiration=2_000_000_000,
        underlying_amount=500_000_000_000,
        collateral="0.5 XCH",
        live_offers=1,
    )


Snap = options_runner.Snapshot


@pytest.mark.parametrize(
    ("snap", "kwargs", "on", "text", "tone"),
    [
        (None, {}, False, "loading", "idle"),
        (Snap(), {}, False, "off", "idle"),
        (
            Snap(contracts=(_contract(), _contract(store.CLOSED))),
            {},
            False,
            "off -- 1 contract open",
            "idle",
        ),
        (Snap(mode=Mode.LIVE, running=True), {}, True, "live", "ok"),
        (
            Snap(mode=Mode.LIVE, running=True, contracts=(_contract(), _contract(store.SOLD))),
            {},
            True,
            "live -- 2 contracts open",
            "ok",
        ),
        (
            Snap(mode=Mode.LIVE, running=True, held="refused, trading engine up"),
            {},
            True,
            "live -- held",
            "warn",
        ),
        (
            Snap(mode=Mode.REHEARSE, running=True),
            {},
            True,
            "rehearsing -- signs nothing",
            "converging",
        ),
        (
            Snap(mode=Mode.REHEARSE, running=True, held="step failed: boom"),
            {},
            True,
            "rehearsing -- held",
            "warn",
        ),
        (
            Snap(running=True, stopping=True),
            {},
            False,
            "stopping -- finishing a step",
            "converging",
        ),
        (Snap(), {"setup": "options are not set up"}, False, "not set up", "idle"),
        (Snap(), {"refusal": "Live refused: the engine is RUNNING"}, False, "refused", "warn"),
        (Snap(start_error="could not start: boom"), {}, False, "could not start", "warn"),
    ],
)
def test_the_toolbar_chip_says_what_the_book_is_doing(snap, kwargs, on, text, tone):
    got_on, chip = options_page.header_status(snap, **kwargs)
    assert (got_on, chip.text, chip.tone) == (on, text, tone)
    assert chip.tooltip


def test_an_off_book_never_claims_no_contracts_are_open():
    """The record is read only when the book steps: off since launch, it does not know."""
    _on, chip = options_page.header_status(Snap())
    assert "no contract" not in chip.tooltip.lower() and chip.text == "off"


def test_the_toolbar_switch_asks_the_same_question_before_going_live(app, tmp_path, monkeypatch):
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner)
    try:
        asked = _answer(monkeypatch, QMessageBox.StandardButton.No)
        assert page.switch_from_header(True) == ""
        assert asked == ["Options: go Live"] and runner.starts == []
        assert page.header_status()[0] is False

        _answer(monkeypatch, QMessageBox.StandardButton.Yes)
        assert page.switch_from_header(True) == ""
        assert runner.starts == [Mode.LIVE]
        on, chip = page.header_status()
        assert on and chip.text == "live"
        assert page._buttons[Mode.LIVE].isChecked(), "the page shows what the toolbar did"
        store_ = QSettings("XOP-tests-options", options_page._SETTINGS_APP)
        assert store_.value(options_page.KEY_MODE) == "live"

        assert page.switch_from_header(False) == ""
        assert runner.stops == 1 and page.header_status()[0] is False
        assert store_.value(options_page.KEY_MODE) == "off"
    finally:
        _close(page, app)


def test_a_live_book_is_not_asked_again(app, tmp_path, monkeypatch):
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner)
    try:
        _answer(monkeypatch, QMessageBox.StandardButton.Yes)
        page.switch_from_header(True)
        asked = _answer(monkeypatch, QMessageBox.StandardButton.Yes)
        assert page.switch_from_header(True) == ""
        assert asked == [] and runner.starts == [Mode.LIVE]
    finally:
        _close(page, app)


def test_a_refused_toolbar_switch_names_the_reason_and_stays_off(app, tmp_path, monkeypatch):
    runner = RunnerDouble(refuse="Live refused: the trading engine is RUNNING")
    page = _page(app, tmp_path, runner)
    try:
        _answer(monkeypatch, QMessageBox.StandardButton.Yes)
        assert "Live refused" in page.switch_from_header(True)
        on, chip = page.header_status()
        assert not on and chip.text == "refused" and "Live refused" in chip.tooltip
        assert page._buttons[Mode.OFF].isChecked()

        page.switch_from_header(False)  # an explicit Off clears it
        assert page.header_status()[1].text == "off"
    finally:
        _close(page, app)


@pytest.mark.parametrize("from_toolbar", [True, False])
def test_answering_no_after_a_refusal_reports_no_refusal(app, tmp_path, monkeypatch, from_toolbar):
    """A declined retry is the operator's own choice: the old reason may no longer be true."""
    runner = RunnerDouble(refuse="Live refused: the trading engine is RUNNING")
    page = _page(app, tmp_path, runner)
    try:
        _answer(monkeypatch, QMessageBox.StandardButton.Yes)
        assert "Live refused" in page.switch_from_header(True)

        asked = _answer(monkeypatch, QMessageBox.StandardButton.No)
        if from_toolbar:
            assert page.switch_from_header(True) == ""
        else:
            page._buttons[Mode.LIVE].click()
        assert asked == ["Options: go Live"]
        assert page.header_status()[1].text == "off"
    finally:
        _close(page, app)


def test_the_toolbar_switch_does_not_ask_when_options_are_not_set_up(app, tmp_path, monkeypatch):
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner, config={"options": False})
    try:
        asked = _answer(monkeypatch, QMessageBox.StandardButton.Yes)
        assert "options.enabled is not true" in page.switch_from_header(True)
        assert asked == [] and runner.starts == []
        assert page.header_status()[1].text == "not set up"
    finally:
        _close(page, app)


def test_the_toolbar_switch_waits_for_the_config(app, monkeypatch):
    runner = RunnerDouble()
    page = options_page.OptionsWidget(runner=runner)
    try:
        asked = _answer(monkeypatch, QMessageBox.StandardButton.Yes)
        assert page.switch_from_header(True) == "the options book is not loaded yet"
        assert asked == [] and runner.starts == []
        assert page.header_status() == (False, options_page.header_status(None)[1])
    finally:
        _close(page, app)


def test_a_rehearsing_book_reads_on_and_the_toolbar_switch_turns_it_off(app, tmp_path):
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner)
    try:
        page._buttons[Mode.REHEARSE].click()
        on, chip = page.header_status()
        assert on and chip.text == "rehearsing -- signs nothing"
        page.switch_from_header(False)
        assert runner.stops == 1 and not page.header_status()[0]
    finally:
        _close(page, app)


def test_a_launch_that_could_not_resume_shows_on_the_toolbar_chip(app, tmp_path, monkeypatch):
    QSettings("XOP-tests-options", options_page._SETTINGS_APP).setValue(
        options_page.KEY_MODE, "live"
    )
    runner = RunnerDouble(refuse="Live refused: the trading engine is RUNNING")
    page = _page(app, tmp_path, runner)
    try:
        on, chip = page.header_status()
        assert not on and chip.text == "refused"
        assert chip.tooltip.startswith("Not resumed as when the app last closed")
    finally:
        _close(page, app)


def test_every_refresh_tells_the_toolbar(app, tmp_path):
    runner = RunnerDouble()
    page = _page(app, tmp_path, runner)
    try:
        seen: list[int] = []
        page.status_changed.connect(lambda: seen.append(1))
        page.refresh()
        assert seen == [1]
    finally:
        _close(page, app)


def test_the_options_switch_follows_the_book_not_the_click(app):
    from gui.services.venue_control import StatusChip
    from gui.widgets.venue_switch import OptionsSwitch

    state = {"on": False}
    asked: list[bool] = []

    def status():
        on = state["on"]
        return on, StatusChip("live" if on else "off", "ok" if on else "idle", "tip")

    switch = OptionsSwitch(status)
    switch.toggleRequested.connect(asked.append)
    try:
        assert not switch.is_on() and switch.status_text() == "off"
        switch.click()
        assert asked == [True]
        assert not switch.is_on(), "a click the book did not take leaves the slider off"
        state["on"] = True
        switch.refresh()
        assert switch.is_on() and switch.status_text() == "live"
        switch.click()
        assert asked == [True, False]
    finally:
        switch.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        app.processEvents()


# -- in the main window ------------------------------------------------------


@pytest.fixture
def real_options_switch_setting():
    """Put the operator's own "options/toolbar_switch" back, whatever a test wrote."""
    settings = QSettings("XOP", "XOPTrader")
    settings.sync()
    saved = settings.value("options/toolbar_switch")
    yield
    restore = QSettings("XOP", "XOPTrader")
    if saved is None:
        restore.remove("options/toolbar_switch")
    else:
        restore.setValue("options/toolbar_switch", saved)
    restore.sync()


def _main_window(monkeypatch, *, shown: bool = True):
    from gui.widgets import settings as settings_mod

    monkeypatch.setattr(settings_mod, "load_permuto_enabled", lambda: False)
    monkeypatch.setattr(settings_mod, "load_startup_states", lambda: ("adopt", "off"))
    monkeypatch.setattr(settings_mod, "load_options_switch_visible", lambda: shown)
    from gui.widgets.main_window import MainWindow

    return MainWindow()


def _close_window(window, app) -> None:
    window._bridge = None
    window.close()
    window.deleteLater()
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    app.processEvents()


def test_the_toolbar_shows_an_options_switch_by_default_and_permuto_stays_hidden(app, monkeypatch):
    window = _main_window(monkeypatch)
    try:
        assert window._options_switch is not None
        # The ACTION: a child of a window never shown reports itself hidden anyway.
        assert window._options_switch_action.isVisible()
        assert window._permuto_switch is None and window._permuto_switch_action is None
        # No bridge, so no config.yaml yet: the chip says so rather than guess.
        assert window._options_switch.status_text() == "loading"
        assert not window._options_switch.is_on()
    finally:
        _close_window(window, app)


def test_settings_can_hide_the_options_switch_and_bring_it_back(app, monkeypatch):
    window = _main_window(monkeypatch, shown=False)
    try:
        assert not window._options_switch_action.isVisible()
        window._on_options_switch_visible_changed(True)
        assert window._options_switch_action.isVisible()
        window._on_options_switch_visible_changed(False)
        assert not window._options_switch_action.isVisible()
    finally:
        _close_window(window, app)


def test_the_toolbar_switch_drives_the_options_page(app, monkeypatch):
    window = _main_window(monkeypatch)
    try:
        page = window._unwrap(window._options_widget)
        calls: list[bool] = []
        monkeypatch.setattr(page, "switch_from_header", lambda on: calls.append(on) or "")
        window._options_switch.click()
        assert calls == [True]

        monkeypatch.setattr(page, "switch_from_header", lambda on: "Live refused: engine up")
        window._options_switch.click()
        assert "Live refused: engine up" in window.statusBar().currentMessage()
    finally:
        _close_window(window, app)


def test_the_toolbar_switch_repaints_with_the_page(app, monkeypatch):
    window = _main_window(monkeypatch)
    try:
        page = window._unwrap(window._options_widget)
        from gui.services.venue_control import StatusChip

        monkeypatch.setattr(page, "header_status", lambda: (True, StatusChip("live", "ok", "tip")))
        page.status_changed.emit()
        assert window._options_switch.is_on()
        assert window._options_switch.status_text() == "live"
    finally:
        _close_window(window, app)


def test_the_subsystems_box_shows_the_switch_by_default_and_remembers(
    app, monkeypatch, real_options_switch_setting
):
    from gui.widgets import settings as settings_mod

    QSettings("XOP", "XOPTrader").remove("options/toolbar_switch")
    assert settings_mod.load_options_switch_visible() is True
    monkeypatch.setattr(settings_mod, "load_permuto_enabled", lambda: False)
    page = settings_mod.SettingsWidget()
    try:
        seen: list[bool] = []
        page.options_switch_visible_changed.connect(seen.append)
        assert page._options_switch_box.isChecked()
        page._options_switch_box.setChecked(False)
        assert seen == [False]
        assert settings_mod.load_options_switch_visible() is False
        page._options_switch_box.setChecked(True)
        assert seen == [False, True] and settings_mod.load_options_switch_visible() is True
    finally:
        page.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        app.processEvents()


@pytest.mark.parametrize(
    ("stored", "expected"),
    [
        (True, True),
        (False, False),
        ("true", True),
        ("false", False),
        ("0", False),
        ("off", False),
        ("no", False),
        # Anything unreadable shows it: the default.
        ("sideways", True),
        ("", True),
    ],
)
def test_the_switch_loader_reads_every_shape(stored, expected, real_options_switch_setting):
    from gui.widgets.settings import load_options_switch_visible

    settings = QSettings("XOP", "XOPTrader")
    settings.setValue("options/toolbar_switch", stored)
    settings.sync()
    assert load_options_switch_visible() is expected


# --------------------------------------------------------------------------- #
# What the page says about the book while it is off
# --------------------------------------------------------------------------- #


def test_an_expired_contract_still_open_says_what_comes_next():
    item = SimpleNamespace(status=store.OPEN, expiration=1_000)
    assert options_page._status_text(item, now=999) == store.OPEN
    said = options_page._status_text(item, now=1_000)
    assert said.startswith("open, expired") and "next Live step" in said
    closed = SimpleNamespace(status=store.CLOSED, expiration=1_000)
    assert options_page._status_text(closed, now=5_000) == store.CLOSED


def test_the_page_shows_the_record_s_contracts_while_the_book_is_off(app, tmp_path):
    class OnRecord(RunnerDouble):
        def contracts_on_record(self):
            return (
                options_runner.ContractView(
                    slot="C110@1",
                    status=store.OPEN,
                    launcher_id="01" * 32,
                    expiration=1,
                    underlying_amount=500_000_000_000,
                    collateral="0.5 XCH",
                    live_offers=0,
                ),
            )

    page = _page(app, tmp_path, OnRecord())
    try:
        page.refresh()
        assert page._contracts.rowCount() == 1
        assert "expired" in page._contracts.item(0, 1).text()
    finally:
        _close(page, app)


def test_the_runner_reads_the_record_s_contracts(tmp_path):
    config_path = _config(tmp_path)
    _open_contract(config_path)
    views = options_runner.OptionsRunner(config_path).contracts_on_record()
    assert [view.slot for view in views] == ["C110@1"]
    assert views[0].collateral == "0.5 XCH"


def test_the_record_is_read_from_a_copy_never_upgraded_in_place(tmp_path):
    """[review] Display only: looking at an older book's record does not migrate its file."""
    if sqlite3.sqlite_version_info < (3, 35, 0):
        pytest.skip("this SQLite cannot drop a column to make an older record")
    config_path = _config(tmp_path)
    _open_contract(config_path)
    db = config_path.parent / "data" / "options_jobs.db"
    con = sqlite3.connect(db)
    con.execute("ALTER TABLE step_log DROP COLUMN interval")  # as an older version left it
    con.commit()
    con.close()

    views = options_runner.OptionsRunner(config_path).contracts_on_record()

    assert [view.slot for view in views] == ["C110@1"]
    con = sqlite3.connect(db)
    try:
        columns = {row[1] for row in con.execute("PRAGMA table_info(step_log)")}
    finally:
        con.close()
    assert "interval" not in columns


# --------------------------------------------------------------------------- #
# [review] Settings saving the file the book runs on
# --------------------------------------------------------------------------- #


class _SavedOver(RunnerDouble):
    """A runner whose settings a save changed (or not); ``slow``: mid-step when stopped."""

    def __init__(self, *, changed: bool = True, slow: bool = False) -> None:
        super().__init__()
        self.changed = changed
        self.slow = slow
        self.finishing = False

    def stop_for_new_settings(self):
        if self.mode is Mode.OFF or not self.changed:
            return None
        mode = self.mode
        self.stop()
        self.finishing = self.slow
        return mode


def _saved(page, tmp_path) -> None:
    page.attach(tmp_path / "config.yaml", resume=False)  # as MainWindow does after a save


def test_saving_new_settings_restarts_the_running_book_with_them(app, tmp_path, monkeypatch):
    asked = _answer(monkeypatch, QMessageBox.StandardButton.Yes)
    runner = _SavedOver()
    page = _page(app, tmp_path, runner)
    try:
        page._buttons[Mode.LIVE].click()
        assert runner.starts == [Mode.LIVE]

        _saved(page, tmp_path)

        assert runner.stops == 1 and runner.starts == [Mode.LIVE, Mode.LIVE]
        assert runner.mode is Mode.LIVE
        assert asked == ["Options: go Live"], "the restart asks nothing more"
    finally:
        _close(page, app)


def test_the_same_settings_saved_again_leave_the_running_book_alone(app, tmp_path, monkeypatch):
    _answer(monkeypatch, QMessageBox.StandardButton.Yes)
    runner = _SavedOver(changed=False)
    page = _page(app, tmp_path, runner)
    try:
        page._buttons[Mode.LIVE].click()
        _saved(page, tmp_path)
        assert runner.stops == 0 and runner.starts == [Mode.LIVE]
    finally:
        _close(page, app)


def test_a_restart_waits_for_the_step_in_progress_and_off_cancels_it(app, tmp_path):
    runner = _SavedOver(slow=True)
    page = _page(app, tmp_path, runner)
    try:
        page._buttons[Mode.REHEARSE].click()
        _saved(page, tmp_path)
        assert runner.starts == [Mode.REHEARSE], "not while its step is still going"
        assert "once the step in progress is over" in page._held.text()

        runner.finishing = False
        page.refresh()
        assert runner.starts == [Mode.REHEARSE, Mode.REHEARSE]

        _saved(page, tmp_path)  # saved again, mid-step again
        page._buttons[Mode.OFF].click()  # the operator's own choice wins
        runner.finishing = False
        page.refresh()
        assert runner.starts == [Mode.REHEARSE, Mode.REHEARSE] and runner.mode is Mode.OFF
    finally:
        _close(page, app)


def test_settings_that_switch_the_book_off_leave_it_off(app, tmp_path, monkeypatch):
    _answer(monkeypatch, QMessageBox.StandardButton.Yes)
    runner = _SavedOver()
    page = _page(app, tmp_path, runner)
    try:
        page._buttons[Mode.LIVE].click()
        _config(tmp_path, options=False)  # Settings saved the book switched off

        _saved(page, tmp_path)

        assert runner.starts == [Mode.LIVE] and runner.mode is Mode.OFF
        assert (
            QSettings("XOP-tests-options", options_page._SETTINGS_APP).value(options_page.KEY_MODE)
            == "off"
        )
    finally:
        _close(page, app)
