"""The options book on a background thread: the GUI switch's engine."""

from __future__ import annotations

import threading
import time
from pathlib import Path
from typing import Any

import pytest

yaml = pytest.importorskip("yaml")

from gui.services.options import engine_guard, runner, store  # noqa: E402
from gui.services.options.service import StepReport  # noqa: E402

USD = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"
DOWN = engine_guard.EngineState(running=False, certain=True, detail="nothing there")
UP = engine_guard.EngineState(running=True, certain=True, detail="xop_trader.exe process 7")


def _write(folder: Path, *, shared: bool = False, enabled: bool = True, db: str = "") -> Path:
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
