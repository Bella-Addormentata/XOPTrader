"""[AUTO-FENCE] ``engine_fence: auto``: the engine's fence follows the options book.

The app keeps ``options_fence.flag`` beside the engine's database while the
book is in use (switched on, a contract open, a transaction unsettled) and
removes it once the book is off with nothing open. The engine follows the flag
(cpp/tests/test_coin_fence.cpp), and so do the app's own spends.
"""

from __future__ import annotations

import dataclasses
import os
import subprocess
import sys
import threading
from pathlib import Path

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")
yaml = pytest.importorskip("yaml")

from gui.services.options import config, engine_guard, fence, runner, store  # noqa: E402
from gui.services.options.tests.test_fence import (  # noqa: E402
    ADDRESS,
    METRICS,
    NOW,
    TRADING,
    UP,
    USD,
    FakeWallet,
    _parse,
    _shared,
)
from gui.services.options.tests.test_runner import (  # noqa: E402
    DOWN,
    FakeBook,
    _record_open_contract,
    _wait,
)
from gui.services.warp.wallet import WalletClient  # noqa: E402


@pytest.fixture(autouse=True)
def no_books_from_other_tests():
    """The engine check remembers every book this process ran; start each test without any."""
    with runner._BOOKS_LOCK:
        runner._BOOKS.clear()
    yield
    with runner._BOOKS_LOCK:
        runner._BOOKS.clear()


def _config(tmp_path: Path, *, follows: bool = True, db: str = "") -> Path:
    root = {
        "chia": TRADING,
        "options": {
            "enabled": True,
            "dry_run": False,  # the switch decides
            "wallet": _shared(engine_fence="auto" if follows else True),
            "quote_asset_id": USD,
            "contract_xch": 1,
            "database_path": db or str(tmp_path / "book.db"),
        },
    }
    path = tmp_path / "config.yaml"
    path.write_text(yaml.safe_dump(root), encoding="utf-8")
    return path


def _flag(tmp_path: Path) -> Path:
    return tmp_path / "data" / fence.FLAG_NAME


def _builder(made: list[FakeBook]):
    def build(cfg):
        book = FakeBook(cfg)
        made.append(book)
        return book, store.OptionStore(cfg.database_path), None

    return build


# -- config and the flag ------------------------------------------------------------


def test_auto_is_the_fence_following_the_book() -> None:
    for value in ("auto", "AUTO", " Auto "):
        wallet = _parse(_shared(engine_fence=value)).wallet
        assert wallet.engine_fence and wallet.fence_follows_book, value
    wallet = _parse(_shared(engine_fence=True)).wallet
    assert wallet.engine_fence and not wallet.fence_follows_book
    with pytest.raises(config.OptionsConfigError, match="cannot both be on"):
        _parse(_shared(engine_fence="auto", fund_from_wallet=True))
    with pytest.raises(config.OptionsConfigError, match="must be true or false"):
        _parse(_shared(engine_fence="sometimes"))


def test_the_flag_sits_beside_the_engine_s_database(tmp_path) -> None:
    assert fence.flag_path(tmp_path, {}) == tmp_path / "data" / "options_fence.flag"
    assert fence.flag_path(tmp_path, None) == tmp_path / "data" / "options_fence.flag"
    assert (
        fence.flag_path(tmp_path, {"database": {"path": "db/x.db"}})
        == tmp_path / "db" / "options_fence.flag"
    )
    absolute = tmp_path / "elsewhere" / "x.db"
    assert (
        fence.flag_path(tmp_path / "other", {"database": {"path": str(absolute)}})
        == absolute.parent / "options_fence.flag"
    )


def test_set_flag_raises_and_lowers_once(tmp_path) -> None:
    flag = _flag(tmp_path)
    assert fence.set_flag(flag, True, "switched on (live)")
    assert fence.flag_up(flag) and "switched on (live)" in flag.read_text(encoding="utf-8")
    assert not fence.set_flag(flag, True, "again")
    assert fence.set_flag(flag, False)
    assert not fence.flag_up(flag)
    assert not fence.set_flag(flag, False)


def test_fenced_address_reads_auto_too() -> None:
    root = {"options": {"enabled": True, "wallet": _shared(engine_fence="auto")}}
    assert fence.follows_book(root)
    assert fence.fenced_address(root) == (ADDRESS, "xch")
    fixed = {"options": {"enabled": True, "wallet": _shared(engine_fence=True)}}
    assert not fence.follows_book(fixed) and fence.fenced_address(fixed) == (ADDRESS, "xch")
    assert not fence.follows_book({})


def test_the_app_s_own_spends_follow_the_flag() -> None:
    wallet = FakeWallet()
    client = WalletClient({"host": "localhost", "port": 9256}, poster=wallet)
    up = {"value": False}
    client.set_fence(
        fence.WalletFence(client._post, ADDRESS, "xch", active=lambda: up["value"]).coin_ids
    )
    send = {"wallet_id": 1, "amount": 5, "address": "xch1x", "fee": 0}

    client._post("send_transaction", dict(send))
    (down,) = [body for endpoint, body in wallet.calls if endpoint == "send_transaction"]
    assert "excluded_coin_ids" not in down, "down: the options coins are the wallet's again"
    assert not any(endpoint == "get_wallets" for endpoint, _ in wallet.calls)

    up["value"] = True
    client._post("send_transaction", dict(send))
    sent = [body for endpoint, body in wallet.calls if endpoint == "send_transaction"]
    assert wallet.ids_at_the_address() <= set(sent[-1]["excluded_coin_ids"])


# -- the runner keeps the flag ------------------------------------------------------


def test_switching_on_raises_the_flag_and_off_with_nothing_open_lowers_it(tmp_path) -> None:
    made: list[FakeBook] = []
    run = runner.OptionsRunner(
        _config(tmp_path), interval=0.02, build=_builder(made), engine_state=lambda url: DOWN
    )
    run.start(runner.Mode.REHEARSE)
    assert _flag(tmp_path).exists(), "raised before the first step"
    _wait(lambda: bool(made) and made[0].steps >= 1)
    run.stop()
    _wait(lambda: not run.snapshot().running)
    assert not _flag(tmp_path).exists(), "off with nothing open: the options coins trade again"


def test_a_contract_open_keeps_the_flag_up_once_the_book_is_off(tmp_path) -> None:
    db = tmp_path / "book.db"
    _record_open_contract(db)
    made: list[FakeBook] = []
    run = runner.OptionsRunner(
        _config(tmp_path, db=str(db)),
        interval=0.02,
        build=_builder(made),
        engine_state=lambda url: DOWN,
    )
    run.start(runner.Mode.REHEARSE)
    _wait(lambda: bool(made) and made[0].steps >= 1)
    run.stop()
    _wait(lambda: not run.snapshot().running)
    assert _flag(tmp_path).exists()


def test_a_transaction_unsettled_keeps_the_flag_up_too(tmp_path) -> None:
    db = tmp_path / "book.db"
    _record_open_contract(db, verdict=None)  # the mint is still in flight
    book = store.OptionStore(str(db))
    try:
        assert runner.in_use_reason(book)
    finally:
        book.close()
    runner.OptionsRunner(_config(tmp_path, db=str(db)), build=_builder([])).sync_fence()
    assert _flag(tmp_path).exists()


def test_live_beside_a_running_engine_waits_for_its_fence_instead_of_refusing(tmp_path) -> None:
    holds = engine_guard.parse_fence(METRICS, now=NOW)
    seen = {"fence": engine_guard.parse_fence("")}
    made: list[FakeBook] = []
    run = runner.OptionsRunner(
        _config(tmp_path),
        interval=0.02,
        build=_builder(made),
        engine_state=lambda url: UP,
        fence_state=lambda url: seen["fence"],
    )
    try:
        run.start(runner.Mode.LIVE)  # not refused: switching on is what raises the flag
        assert _flag(tmp_path).exists()
        _wait(lambda: "waiting for the trading engine to put its fence up" in run.snapshot().held)
        assert made and made[0].steps == 0, "nothing signs before the engine's fence is up"
        seen["fence"] = holds
        _wait(lambda: made[0].steps >= 1)
        assert run.snapshot().held == ""
    finally:
        run.stop()


def test_the_page_s_check_drops_a_stale_flag_and_raises_one_for_open_contracts(tmp_path) -> None:
    db = tmp_path / "book.db"
    flag = _flag(tmp_path)
    fence.set_flag(flag, True, "left by the last session")
    run = runner.OptionsRunner(_config(tmp_path, db=str(db)), build=_builder([]))
    run.sync_fence()
    assert not flag.exists()
    _record_open_contract(db)
    run.sync_fence()
    assert flag.exists()


def test_a_book_without_auto_leaves_the_flag_alone(tmp_path) -> None:
    flag = _flag(tmp_path)
    fence.set_flag(flag, True, "someone else's")
    made: list[FakeBook] = []
    run = runner.OptionsRunner(
        _config(tmp_path, follows=False),
        interval=0.02,
        build=_builder(made),
        engine_state=lambda url: DOWN,
    )
    run.sync_fence()
    assert flag.exists()
    flag.unlink()
    run.start(runner.Mode.REHEARSE)
    _wait(lambda: bool(made) and made[0].steps >= 1)
    run.stop()
    _wait(lambda: not run.snapshot().running)
    assert not flag.exists(), "engine_fence: true raises no flag"


def test_an_unreadable_record_keeps_the_flag_up(tmp_path) -> None:
    db = tmp_path / "book.db"
    db.write_bytes(b"this is not a database" * 100)
    runner.OptionsRunner(_config(tmp_path, db=str(db)), build=_builder([])).sync_fence()
    assert _flag(tmp_path).exists()


def test_a_run_on_keeps_the_flag_up_whatever_the_record_says(tmp_path) -> None:
    """The check a run makes as it ends must not lower a newer run's flag."""
    path = _config(tmp_path)
    run = runner.OptionsRunner(path, build=_builder([]))
    run._run = runner._Run(
        thread=threading.current_thread(), stop=threading.Event(), mode=runner.Mode.LIVE
    )
    with run._fence_lock:
        run._sync_fence_locked(runner.load_options(path))
    assert _flag(tmp_path).exists()


# -- the command line ------------------------------------------------------------------


def test_the_command_line_raises_the_flag_and_leaves_lowering_to_the_app(
    tmp_path, monkeypatch
) -> None:
    from gui.services.options import __main__ as cli

    path = _config(tmp_path)
    cfg = cli._load(str(path))
    monkeypatch.setattr(engine_guard, "engine_state", lambda url: DOWN)
    cli._raise_fence_flag(cfg, str(path), "step")
    assert _flag(tmp_path).exists()

    _flag(tmp_path).unlink()
    cli._raise_fence_flag(dataclasses.replace(cfg, dry_run=True), str(path), "step")
    assert not _flag(tmp_path).exists(), "a dry run signs nothing and needs no fence"

    monkeypatch.setattr(engine_guard, "fence_state", lambda url: engine_guard.parse_fence(""))
    refusal = cli._engine_refusal(UP, "a live step", cfg)
    assert refusal is not None and "follows options_fence.flag" in refusal


# -- review of #243 ----------------------------------------------------------------------


def test_a_flag_that_cannot_be_written_refuses_live_at_the_switch(tmp_path, monkeypatch) -> None:
    monkeypatch.setattr(fence, "raise_flag", lambda path, why: False)
    run = runner.OptionsRunner(_config(tmp_path), build=_builder([]), engine_state=lambda url: DOWN)
    with pytest.raises(runner.RunnerError, match="fence flag could not be written"):
        run.start(runner.Mode.LIVE)
    assert run.snapshot().mode is runner.Mode.OFF


def test_a_flag_that_cannot_be_written_holds_each_live_step(tmp_path, monkeypatch) -> None:
    """Engine stopped: the metrics guard has nothing to say, so the flag is the only fence."""
    made: list[FakeBook] = []
    run = runner.OptionsRunner(
        _config(tmp_path), interval=0.02, build=_builder(made), engine_state=lambda url: DOWN
    )
    try:
        run.start(runner.Mode.LIVE)
        monkeypatch.setattr(fence, "raise_flag", lambda path, why: False)
        _wait(lambda: "fence flag could not be written" in run.snapshot().held)
        steps = made[0].steps
        _wait(lambda: run.snapshot().steps >= 3)
        assert made[0].steps == steps, "nothing signs without the flag"
    finally:
        run.stop()


def test_a_flag_another_live_process_raised_stays_up(tmp_path) -> None:
    """A run from a terminal, between its steps, before it has anything open."""
    flag = _flag(tmp_path)
    flag.parent.mkdir(parents=True)
    flag.write_text(f"pid={os.getppid()}\nwhy=the options book is in use: run\n", encoding="utf-8")
    runner.OptionsRunner(_config(tmp_path), build=_builder([])).sync_fence()
    assert flag.exists()


def test_a_flag_whose_process_is_gone_comes_down(tmp_path) -> None:
    done = subprocess.run(
        [sys.executable, "-c", "import os; print(os.getpid())"],
        capture_output=True,
        text=True,
        check=True,
    )
    gone = int(done.stdout.strip())
    assert fence.process_gone(gone)
    assert not fence.process_gone(os.getpid()) and not fence.process_gone(os.getppid())
    flag = _flag(tmp_path)
    flag.parent.mkdir(parents=True)
    flag.write_text(f"pid={gone}\n", encoding="utf-8")
    runner.OptionsRunner(_config(tmp_path), build=_builder([])).sync_fence()
    assert not flag.exists()


def test_a_book_mid_step_elsewhere_keeps_the_flag_up(tmp_path) -> None:
    db = tmp_path / "book.db"
    flag = _flag(tmp_path)
    fence.raise_flag(flag, "a step")
    other = store.OptionStore(str(db))
    try:
        with other.acting():  # another process's step, as far as the lock can tell
            runner.OptionsRunner(_config(tmp_path, db=str(db)), build=_builder([])).sync_fence()
            assert flag.exists()
    finally:
        other.close()
    runner.OptionsRunner(_config(tmp_path, db=str(db)), build=_builder([])).sync_fence()
    assert not flag.exists()


def test_the_flag_names_the_process_that_raised_it(tmp_path) -> None:
    flag = _flag(tmp_path)
    assert fence.raise_flag(flag, "switched on (live)")
    assert fence.flag_owner(flag) == os.getpid()
    flag.write_text("written by hand\n", encoding="utf-8")
    assert fence.flag_owner(flag) is None


def test_the_flag_path_expands_as_the_engine_reads_database_path(tmp_path, monkeypatch) -> None:
    monkeypatch.setenv("XOP_TEST_DATA", str(tmp_path / "from-env"))
    monkeypatch.delenv("XOP_TEST_UNSET", raising=False)
    assert (
        fence.flag_path(tmp_path, {"database": {"path": "${XOP_TEST_DATA}/trader.db"}})
        == tmp_path / "from-env" / "options_fence.flag"
    )
    assert (
        fence.flag_path(tmp_path, {"database": {"path": " ${XOP_TEST_UNSET:-db}/x.db "}})
        == tmp_path / "db" / "options_fence.flag"
    )
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    assert (
        fence.flag_path(tmp_path, {"database": {"path": "~/xop/trader.db"}})
        == tmp_path / "home" / "xop" / "options_fence.flag"
    )


def test_live_test_raises_the_flag_even_in_a_dry_run(tmp_path, monkeypatch) -> None:
    """live-test signs whatever options.dry_run says."""
    from gui.services.options import __main__ as cli

    path = _config(tmp_path)
    cfg = dataclasses.replace(cli._load(str(path)), dry_run=True)
    monkeypatch.setattr(engine_guard, "engine_state", lambda url: DOWN)
    assert cli._raise_fence_flag(cfg, str(path), "live-test") is None
    assert _flag(tmp_path).exists()

    monkeypatch.setattr(fence, "raise_flag", lambda path, why: False)
    refusal = cli._raise_fence_flag(dataclasses.replace(cfg, dry_run=False), str(path), "step")
    assert refusal is not None and "could not be written" in refusal
