"""The options book on a background thread: the GUI switch's engine."""

from __future__ import annotations

import logging
import threading
import time
from pathlib import Path
from typing import Any

import pytest

yaml = pytest.importorskip("yaml")

from gui.services.options import engine_guard, runner, store  # noqa: E402
from gui.services.options.service import ENGINE_REFUSED, StepReport  # noqa: E402

USD = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"
DOWN = engine_guard.EngineState(running=False, certain=True, detail="nothing there")
UP = engine_guard.EngineState(running=True, certain=True, detail="xop_trader.exe process 7")


@pytest.fixture(autouse=True)
def no_books_from_other_tests():
    """The engine check remembers every book this process ran; start each test without any."""
    with runner._BOOKS_LOCK:
        runner._BOOKS.clear()
    yield
    with runner._BOOKS_LOCK:
        runner._BOOKS.clear()


def _write(
    folder: Path,
    *,
    shared: bool = False,
    enabled: bool = True,
    db: str = "",
    wallet_extra: dict[str, Any] | None = None,
) -> Path:
    wallet: dict[str, Any] = {"port": 9356, "cert_path": "c", "key_path": "k"}
    chia: dict[str, Any] = {}
    if shared:
        wallet = {
            "port": 9256,
            "cert_path": "c",
            "key_path": "k",
            "fingerprint": 42,
            "share_trading_wallet": True,
            "address": "xch1kxxd76v9kwsxrcxgndn3mylnxjhd7klgdns7083zjl247ffge32sr5rsy2",
        }
        chia = {"wallet_host": "localhost", "wallet_port": 9256, "wallet_fingerprint": 42}
    wallet.update(wallet_extra or {})
    root = {
        "chia": chia,
        "options": {
            "enabled": enabled,
            "dry_run": False,  # the switch decides, not this
            "wallet": wallet,
            "quote_asset_id": USD,
            "contract_xch": 1,
            "database_path": db or "data/options_jobs.db",
        },
    }
    path = folder / "config.yaml"
    path.write_text(yaml.safe_dump(root), encoding="utf-8")
    return path


class FakeBook:
    def __init__(self, cfg: Any, fail: bool = False) -> None:
        self.cfg = cfg
        self.steps = 0
        self.fail = fail
        self.thread: str | None = None
        self.journaled: list[tuple[str, str]] = []

    def journal(self, verdict: str, detail: str = "", market: Any = None, *, now: int) -> None:
        self.journaled.append((verdict, detail))

    def step(self) -> StepReport:
        self.thread = threading.current_thread().name
        self.steps += 1
        if self.fail:
            raise RuntimeError("wallet unreachable")
        report = StepReport(dry_run=self.cfg.dry_run)
        report.did("dry run: would mint C110" if self.cfg.dry_run else "minting C110")
        return report


def _builder(made: list[FakeBook], fail: bool = False):
    def build(cfg: Any) -> tuple[Any, Any, Any]:
        book = FakeBook(cfg, fail)
        made.append(book)
        # Opened on the runner's own thread, as the real build does.
        return book, store.OptionStore(cfg.database_path), None

    return build


def _wait(predicate, seconds: float = 5.0) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("timed out")


def test_options_that_are_not_set_up_cannot_be_switched_on(tmp_path) -> None:
    path = _write(tmp_path, enabled=False)
    with pytest.raises(runner.RunnerError, match="options.enabled is not true"):
        runner.load_options(path)
    with pytest.raises(runner.RunnerError):
        runner.OptionsRunner(path, build=_builder([])).start(runner.Mode.REHEARSE)


def test_a_relative_database_is_read_beside_config_yaml_not_the_program(tmp_path) -> None:
    cfg = runner.load_options(_write(tmp_path))
    assert Path(cfg.database_path) == tmp_path / "data" / "options_jobs.db"
    absolute = runner.load_options(_write(tmp_path, db=str(tmp_path / "x.db")))
    assert Path(absolute.database_path) == tmp_path / "x.db"


def test_rehearse_is_always_a_dry_run_and_steps_on_its_own_thread(tmp_path) -> None:
    made: list[FakeBook] = []
    run = runner.OptionsRunner(_write(tmp_path, db=":memory:"), interval=0.02, build=_builder(made))

    run.start(runner.Mode.REHEARSE)
    _wait(lambda: run.snapshot().steps >= 3)
    run.stop()

    (book,) = made
    assert book.cfg.dry_run is True  # options.dry_run said false; the switch says rehearse
    assert book.thread == "options-rehearse"
    snap = run.snapshot()
    assert snap.mode is runner.Mode.OFF and not snap.running
    assert any("would mint" in line for line in snap.recent)
    assert any(line.endswith("switched off") for line in snap.recent)
    steps = book.steps
    time.sleep(0.1)
    assert book.steps == steps  # stopped means stopped


def test_live_is_live_only_because_it_was_chosen(tmp_path) -> None:
    made: list[FakeBook] = []
    run = runner.OptionsRunner(_write(tmp_path, db=":memory:"), interval=0.02, build=_builder(made))
    run.start(runner.Mode.LIVE)
    _wait(lambda: run.snapshot().steps >= 1)
    run.start(runner.Mode.REHEARSE)  # switching modes restarts with the other setting
    _wait(lambda: len(made) == 2 and made[1].steps >= 1)
    run.stop()
    assert [book.cfg.dry_run for book in made] == [False, True]


@pytest.mark.parametrize("engine", [UP, engine_guard.EngineState(False, False, "tasklist failed")])
def test_live_cannot_be_switched_on_while_a_shared_wallet_s_engine_may_be_up(
    tmp_path, engine
) -> None:
    made: list[FakeBook] = []
    run = runner.OptionsRunner(
        _write(tmp_path, shared=True, db=":memory:"),
        interval=0.02,
        build=_builder(made),
        engine_state=lambda url: engine,
    )
    with pytest.raises(runner.RunnerError, match="Live refused"):
        run.start(runner.Mode.LIVE)
    assert made == [] and run.snapshot().mode is runner.Mode.OFF
    # A rehearsal signs nothing, so it may run beside the engine.
    run.start(runner.Mode.REHEARSE)
    _wait(lambda: run.snapshot().steps >= 1)
    run.stop()
    assert made[0].steps >= 1


def test_an_engine_started_during_a_live_run_holds_every_step(tmp_path) -> None:
    made: list[FakeBook] = []
    state = {"now": DOWN}
    run = runner.OptionsRunner(
        _write(tmp_path, shared=True, db=":memory:"),
        interval=0.02,
        build=_builder(made),
        engine_state=lambda url: state["now"],
    )
    run.start(runner.Mode.LIVE)
    _wait(lambda: made and made[0].steps >= 1)
    state["now"] = UP
    _wait(lambda: run.snapshot().held.startswith("refused, trading engine up"))
    stepped = made[0].steps
    time.sleep(0.1)
    run.stop()
    assert made[0].steps == stepped  # no step while the engine is up
    assert "RUNNING" in run.snapshot().engine
    # Journaled as the command line journals it, for the report's held-back time.
    assert (ENGINE_REFUSED, UP.describe()) in made[0].journaled


def test_a_failing_step_is_reported_and_the_loop_keeps_going(tmp_path) -> None:
    made: list[FakeBook] = []
    run = runner.OptionsRunner(
        _write(tmp_path, db=":memory:"), interval=0.02, build=_builder(made, fail=True)
    )
    run.start(runner.Mode.REHEARSE)
    _wait(lambda: run.snapshot().steps >= 3)
    run.stop()
    assert run.snapshot().held == "step failed: wallet unreachable"


def test_a_book_that_cannot_start_puts_the_switch_back_off(tmp_path) -> None:
    def broken(cfg: Any) -> Any:
        raise RuntimeError("certificate not found")

    run = runner.OptionsRunner(_write(tmp_path, db=":memory:"), interval=0.02, build=broken)
    run.start(runner.Mode.REHEARSE)
    _wait(lambda: not run.snapshot().running)
    snap = run.snapshot()
    assert snap.mode is runner.Mode.OFF and "certificate not found" in snap.held


def _record_minting_contract(db: Path) -> None:
    """One contract whose mint is on its way: collateral is as good as locked."""
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


def test_the_engine_may_start_unless_a_shared_book_has_contracts_open(tmp_path) -> None:
    assert runner.engine_start_blocker(tmp_path / "missing.yaml") is None  # no options at all
    (tmp_path / "own").mkdir()
    dedicated = _write(tmp_path / "own")
    _record_minting_contract(tmp_path / "own" / "data" / "options_jobs.db")
    assert runner.engine_start_blocker(dedicated) is None  # its own wallet: never in the way

    (tmp_path / "shared").mkdir()
    shared = _write(tmp_path / "shared", shared=True)
    assert runner.engine_start_blocker(shared) is None  # no record yet: nothing open
    _record_minting_contract(tmp_path / "shared" / "data" / "options_jobs.db")
    why = runner.engine_start_blocker(shared)
    assert why is not None and "1 contract(s) open (0.5 XCH locked)" in why
    with runner.engine_start_window(shared) as why:
        assert why is not None and "contract(s) open" in why


# -- review of #235 ------------------------------------------------------------------


def test_the_engine_waits_while_a_step_holds_the_book(tmp_path) -> None:
    # A step that is minting holds the acting lock; the engine must not launch
    # between "nothing is open" and that mint landing.
    shared = _write(tmp_path, shared=True)
    db = tmp_path / "data" / "options_jobs.db"
    db.parent.mkdir(parents=True)
    stepping = store.OptionStore(str(db))
    try:
        with stepping.acting():
            with runner.engine_start_window(shared) as why:
                assert why is not None and "in the middle of a step" in why
        with runner.engine_start_window(shared) as why:
            assert why is None  # nothing open, nobody stepping: it may launch...
            with pytest.raises(store.BusyError):  # ...and no step can act until it has
                with stepping.acting():
                    pass
    finally:
        stepping.close()


def test_a_live_book_on_the_shared_wallet_holds_the_engine_with_nothing_open_yet(
    tmp_path,
) -> None:
    made: list[FakeBook] = []
    path = _write(tmp_path, shared=True, db=":memory:")
    run = runner.OptionsRunner(
        path, interval=0.02, build=_builder(made), engine_state=lambda url: DOWN
    )
    run.start(runner.Mode.LIVE)
    try:
        why = runner.engine_start_blocker(path)
        assert why is not None and "is Live" in why
        with runner.engine_start_window(path) as why:
            assert why is not None and "is Live" in why
    finally:
        run.stop()
    assert runner.engine_start_blocker(path) is None  # off, and nothing open


def test_a_shared_book_whose_settings_cannot_be_read_holds_the_engine(tmp_path) -> None:
    path = _write(tmp_path, shared=True)
    root = yaml.safe_load(path.read_text())
    del root["options"]["wallet"]["cert_path"]  # e.g. secrets.yaml unreadable
    path.write_text(yaml.safe_dump(root))
    why = runner.engine_start_blocker(path)
    assert why is not None and "could not be read" in why
    # The same mistake on a book with a wallet of its own holds nothing.
    other = tmp_path / "own"
    other.mkdir()
    own = _write(other)
    root = yaml.safe_load(own.read_text())
    del root["options"]["wallet"]["cert_path"]
    own.write_text(yaml.safe_dump(root))
    assert runner.engine_start_blocker(own) is None
    # ...as does a book that is switched off.
    off = tmp_path / "off"
    off.mkdir()
    assert runner.engine_start_blocker(_write(off, shared=True, enabled=False)) is None


def test_a_book_on_a_config_left_behind_still_holds_the_engine(tmp_path) -> None:
    # Settings switched config.yaml: the check must still see the book run on
    # the old one, which kept its contracts.
    (tmp_path / "a").mkdir()
    (tmp_path / "b").mkdir()
    first = _write(tmp_path / "a", shared=True)
    second = _write(tmp_path / "b", enabled=False)
    run = runner.OptionsRunner(first, interval=0.02, build=_builder([]))
    run.start(runner.Mode.REHEARSE)
    run.stop()
    _record_minting_contract(tmp_path / "a" / "data" / "options_jobs.db")
    why = runner.engine_start_blocker(second)
    assert why is not None and "contract(s) open" in why


# -- options.wallet.engine_beside_open_contracts ------------------------------------

BESIDE = {"engine_beside_open_contracts": True, "fund_from_wallet": True}


def _record_open_contract(
    db: Path, *, valid_until: int | None = None, verdict: str | None = "confirmed"
) -> None:
    """One minted contract; its mint ``verdict`` (None: the mint is still in flight)."""
    db.parent.mkdir(parents=True, exist_ok=True)
    book = store.OptionStore(str(db))
    try:
        mint = book.add_contract(
            launcher_id=b"\x03" * 32,
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
            valid_until=valid_until,
            now=0,
        )
        if verdict is not None:
            book.settle_broadcast(mint, verdict, contract_status=store.OPEN)
    finally:
        book.close()


def test_beside_open_contracts_the_engine_may_start_while_contracts_sit_open(tmp_path) -> None:
    db = tmp_path / "book.db"
    _record_open_contract(db)
    (tmp_path / "beside").mkdir()
    beside = _write(tmp_path / "beside", shared=True, db=str(db), wallet_extra=BESIDE)
    assert runner.engine_start_blocker(beside) is None
    with runner.engine_start_window(beside) as why:
        assert why is None
    # The same record under the default rule still holds it.
    (tmp_path / "strict").mkdir()
    strict = _write(tmp_path / "strict", shared=True, db=str(db))
    why = runner.engine_start_blocker(strict)
    assert why is not None and "1 contract(s) open (0.5 XCH locked)" in why


@pytest.mark.parametrize(
    ("valid_for", "verdict", "holds"),
    [
        (3600, None, True),  # a mint still in flight
        (3600, "stalled", True),  # stalled can still confirm
        (None, None, True),  # no valid_until at all
        # Past valid_until by this machine's clock, long ago or just now: only
        # a verdict the book reads from the chain's own clock releases it.
        (-60, None, True),
        (-86_400, None, True),
        (3600, "confirmed", False),  # it confirmed: its coins are spent
        (3600, "conflict", False),  # overtaken or lapsed: its coins are free
    ],
)
def test_beside_open_contracts_the_book_s_transactions_in_flight_hold_the_engine(
    tmp_path, valid_for, verdict, holds
) -> None:
    db = tmp_path / "book.db"
    until = None if valid_for is None else int(time.time()) + valid_for
    _record_open_contract(db, valid_until=until, verdict=verdict)
    path = _write(tmp_path, shared=True, db=str(db), wallet_extra=BESIDE)
    why = runner.engine_start_blocker(path)
    with runner.engine_start_window(path) as in_window:
        pass
    if holds:
        assert why is not None and "1 transaction(s) waiting to confirm (mint)" in why
        assert "until its record shows them settled" in why
        assert in_window == why
    else:
        assert why is None and in_window is None


def test_beside_open_contracts_live_and_a_step_still_hold_the_engine(tmp_path) -> None:
    db = tmp_path / "book.db"
    _record_open_contract(db)
    path = _write(tmp_path, shared=True, db=str(db), wallet_extra=BESIDE)
    # A step in progress holds the acting lock: the engine must not launch under it.
    stepping = store.OptionStore(str(db))
    try:
        with stepping.acting():
            with runner.engine_start_window(path) as why:
                assert why is not None and "in the middle of a step" in why
    finally:
        stepping.close()
    # Live holds it, open contracts or not.
    (tmp_path / "live").mkdir()
    live = _write(tmp_path / "live", shared=True, db=":memory:", wallet_extra=BESIDE)
    run = runner.OptionsRunner(
        live, interval=0.02, build=_builder([]), engine_state=lambda url: DOWN
    )
    run.start(runner.Mode.LIVE)
    try:
        why = runner.engine_start_blocker(live)
        assert why is not None and "is Live" in why
    finally:
        run.stop()


def test_beside_open_contracts_live_is_still_refused_while_the_engine_runs(tmp_path) -> None:
    db = tmp_path / "book.db"
    _record_open_contract(db)
    path = _write(tmp_path, shared=True, db=str(db), wallet_extra=BESIDE)
    run = runner.OptionsRunner(path, interval=0.02, build=_builder([]), engine_state=lambda url: UP)
    with pytest.raises(runner.RunnerError, match="Live refused"):
        run.start(runner.Mode.LIVE)
    assert run.mode is runner.Mode.OFF


def test_a_stop_that_outlives_its_wait_keeps_the_run_and_blocks_a_new_one(tmp_path) -> None:
    release = threading.Event()
    entered = threading.Event()

    class SlowBook(FakeBook):
        def step(self) -> StepReport:
            entered.set()
            release.wait(5)
            return super().step()

    made: list[FakeBook] = []

    def build(cfg: Any) -> tuple[Any, Any, Any]:
        book = SlowBook(cfg)
        made.append(book)
        return book, store.OptionStore(cfg.database_path), None

    run = runner.OptionsRunner(_write(tmp_path, db=":memory:"), interval=0.02, build=build)
    run.start(runner.Mode.LIVE)
    assert entered.wait(5)
    run.stop(timeout=0.05)  # the step is still running
    snap = run.snapshot()
    assert snap.mode is runner.Mode.OFF and snap.stopping and snap.running
    with pytest.raises(runner.RunnerError, match="still finishing its step"):
        run.start(runner.Mode.REHEARSE)
    assert len(made) == 1  # no second run beside the first
    release.set()
    _wait(lambda: not run.snapshot().running)
    run.start(runner.Mode.REHEARSE)  # now it may
    _wait(lambda: len(made) == 2)
    run.stop()


def test_collateral_is_shown_in_its_own_asset(tmp_path) -> None:
    class Contract:
        def __init__(self, slot, asset, amount):
            self.slot, self.status, self.launcher_id = slot, store.OPEN, b"\x01" * 32
            self.expiration, self.underlying_asset, self.underlying_amount = 1, asset, amount

    class FakeStore:
        def contracts(self):
            return [
                Contract("C110", None, 500_000_000_000),
                Contract("P90", bytes.fromhex(USD), 125_000),  # 125 quote, 3 decimals
            ]

        def offers(self, launcher_id, status):
            return []

        def broadcasts(self, status):
            return []

    run = runner.OptionsRunner(_write(tmp_path, db=":memory:"), build=_builder([]))
    cfg = runner.load_options(_write(tmp_path, db=":memory:"))
    run._record(cfg, FakeStore(), None, "", "")
    assert [c.collateral for c in run.snapshot().contracts] == ["0.5 XCH", "125 quote"]


def test_the_command_line_reads_a_relative_book_beside_config_yaml_too(
    tmp_path, monkeypatch
) -> None:
    from gui.services.options import __main__ as cli
    from gui.services.options import config

    home = tmp_path / "home"
    elsewhere = tmp_path / "elsewhere"
    home.mkdir()
    elsewhere.mkdir()
    path = _write(home)
    monkeypatch.chdir(elsewhere)
    assert Path(cli._load(str(path)).database_path) == home / "data" / "options_jobs.db"
    # A book only where the old rule put it (the folder the command ran from)
    # is not silently replaced by a new one beside config.yaml.
    (elsewhere / "data").mkdir()
    (elsewhere / "data" / "options_jobs.db").write_bytes(b"")
    with pytest.raises(config.OptionsConfigError, match="Move it beside config.yaml"):
        cli._load(str(path))


def test_a_run_from_the_app_writes_the_book_s_own_log_file(tmp_path, caplog) -> None:
    made: list[FakeBook] = []
    run = runner.OptionsRunner(_write(tmp_path), interval=0.02, build=_builder(made))
    package = logging.getLogger("gui.services.options")
    handlers = list(package.handlers)

    with caplog.at_level(logging.INFO, logger="gui.services.options"):
        run.start(runner.Mode.REHEARSE)
        _wait(lambda: made and made[0].steps >= 2)
        run.stop()

    # Beside the database, as the command line writes it, with a time on every line.
    lines = (tmp_path / "data" / "options_jobs.log").read_text(encoding="utf-8").splitlines()
    assert "rehearse run started from the app" in lines[0]
    assert sum("dry run: would mint C110" in line for line in lines) >= 2
    assert lines[-1].endswith("[options] rehearse run stopped")
    assert all(line[:4].isdigit() and line[4] == "-" for line in lines)
    assert package.handlers == handlers  # the file is let go with the run
    assert made[0].step_interval == 0.02  # the journal keeps it, for the report
