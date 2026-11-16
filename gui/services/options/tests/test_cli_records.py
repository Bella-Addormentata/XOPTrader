"""What the command line leaves behind and says: the log file, the report
command, the shared-wallet banner, a refused step, and whether status can tell
that a cancel would be funded."""

from __future__ import annotations

import logging
import re
import time
from pathlib import Path
from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")
yaml = pytest.importorskip("yaml")

from gui.services.options import __main__ as cli  # noqa: E402
from gui.services.options import runtime, store  # noqa: E402
from gui.services.options.service import ENGINE_REFUSED, REPORTED  # noqa: E402
from gui.services.options.tests.sim_world import CAT, World  # noqa: E402
from gui.services.options.tests.test_runtime import (  # noqa: E402
    _DOWN,
    _SHARED_WALLET,
    _TRADING,
    _UP,
    WalletRpc,
    _config,
    _engine,
)

USD = b"\x07" * 32
_STAMP = re.compile(r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}[+-]\d{4} ")
_EXPIRY = 1_794_787_200  # 2026-11-16 00:00 UTC


def _config_file(tmp_path: Path) -> tuple[str, Path]:
    db = tmp_path / "data" / "options_jobs.db"
    section = {
        "enabled": True,
        "dry_run": True,
        "network": "testnet11",
        "wallet": {"port": 9356, "cert_path": "c", "key_path": "k"},
        "quote_asset_id": USD.hex(),
        "contract_xch": 20,
        "database_path": str(db),
    }
    path = tmp_path / "config.yaml"
    path.write_text(yaml.safe_dump({"options": section}))
    return str(path), db


def _logging_step(cfg: Any) -> int:
    """A step that logs as the book does: a reported action, and a warning of its own."""
    service = logging.getLogger("gui.services.options.service")
    service.info("[options] offering C110 at 0.005", extra=REPORTED)
    service.warning("[options] the step journal could not be written: disk full")
    return 0


def _shared(tmp_path: Path, **overrides: Any) -> Any:
    wallet = {**_SHARED_WALLET, "address": sdk.Address(b"\x01" * 32, "txch").encode()}
    return _config(tmp_path, USD, chia=_TRADING, wallet=wallet, **overrides)


# -- the log file -------------------------------------------------------------


def test_the_log_file_sits_beside_the_database_with_a_time_on_every_line(
    tmp_path, monkeypatch, capsys
) -> None:
    path, db = _config_file(tmp_path)
    monkeypatch.setattr(cli, "_step", _logging_step)
    root = logging.getLogger()
    handlers, level = list(root.handlers), root.level

    assert cli.main(["--config", path, "step"]) == 0

    lines = (db.parent / "options_jobs.log").read_text(encoding="utf-8").splitlines()
    assert len(lines) == 3 and all(_STAMP.match(line) for line in lines)
    assert "INFO    gui.services.options.__main__: [options] step, config " in lines[0]
    assert lines[1].endswith("gui.services.options.service: [options] offering C110 at 0.005")
    assert "WARNING" in lines[2] and lines[2].endswith("could not be written: disk full")
    # The terminal still gets the warning, but not what the printed report says.
    err = capsys.readouterr().err
    assert "disk full" in err and "offering C110" not in err
    assert (root.handlers, root.level) == (handlers, level)  # nothing left behind


def test_the_log_file_can_be_put_elsewhere_or_turned_off(tmp_path, monkeypatch) -> None:
    path, db = _config_file(tmp_path)
    monkeypatch.setattr(cli, "_step", _logging_step)
    elsewhere = tmp_path / "logs" / "book.log"

    assert cli.main(["--config", path, "--log-file", str(elsewhere), "step"]) == 0
    assert cli.main(["--config", path, "--no-log-file", "step"]) == 0

    assert len(elsewhere.read_text(encoding="utf-8").splitlines()) == 3  # the first run only
    assert not (db.parent / "options_jobs.log").exists()


def test_commands_that_only_look_write_no_log_file(tmp_path, monkeypatch) -> None:
    path, db = _config_file(tmp_path)
    store.OptionStore(db).close()
    monkeypatch.setattr(cli, "_status", lambda cfg: 0)

    assert cli.main(["--config", path, "status"]) == 0
    assert cli.main(["--config", path, "report"]) == 0

    assert not (db.parent / "options_jobs.log").exists()


def test_an_error_is_shown_once_and_kept_in_the_log_file(tmp_path, monkeypatch, capsys) -> None:
    path, db = _config_file(tmp_path)

    def broken(cfg: Any) -> int:
        raise RuntimeError("wallet certificate not found")

    monkeypatch.setattr(cli, "_step", broken)

    assert cli.main(["--config", path, "step"]) == 1

    assert capsys.readouterr().err.count("wallet certificate not found") == 1
    text = (db.parent / "options_jobs.log").read_text(encoding="utf-8")
    assert "ERROR   gui.services.options.__main__: [options] wallet certificate not found" in text


def test_the_log_is_named_after_the_database(tmp_path) -> None:
    assert cli.default_log_path(_config(tmp_path, USD)) == tmp_path / "options_jobs.log"
    assert cli.default_log_path(_config(tmp_path, USD, database_path=":memory:")) is None


def test_the_terminal_leaves_out_only_what_the_printed_report_says() -> None:
    plain = logging.LogRecord("x", logging.WARNING, __file__, 1, "plain", None, None)
    reported = logging.LogRecord("x", logging.WARNING, __file__, 1, "reported", None, None)
    reported.options_report = True

    keep = cli._NotReported()  # noqa: SLF001

    assert keep.filter(plain) and not keep.filter(reported)


# -- the report command -------------------------------------------------------


def test_the_report_command_prints_the_book_and_writes_a_csv(tmp_path, capsys) -> None:
    path, db = _config_file(tmp_path)
    book = store.OptionStore(db)
    start = int(time.time()) - 7200
    book.add_contract(
        launcher_id=b"\x11" * 32,
        slot="C110@5",
        side="call",
        underlying_asset=None,
        underlying_amount=10**12,
        strike_asset=USD,
        strike_amount=1250,
        expiration=_EXPIRY,
        creator_puzzle_hash=b"\x33" * 32,
        bundle=b"x",
        deadline=start + 600,
        now=start,
    )
    book.log_step(store.QUOTING, now=start + 60)
    book.close()
    sheet = tmp_path / "report.csv"

    assert cli.main(["--config", path, "report", "--since", "all", "--csv", str(sheet)]) == 0

    out = capsys.readouterr().out
    # "all" starts at the first thing the book recorded, not in 1970.
    assert out.startswith(f"Options book, {time.strftime('%Y-%m-%d %H:%M', time.gmtime(start))}")
    assert "C110@5" in out and "minting" in out
    assert f"1 contract row(s) written to {sheet}" in out
    assert sheet.read_text(encoding="utf-8").startswith("launcher_id,slot,side,")


@pytest.mark.parametrize(
    ("since", "error"),
    [("yesterday", "give a span like 24h or 7d"), ("2099-01-01", "is in the future")],
)
def test_a_report_period_that_cannot_be_used_is_refused(tmp_path, capsys, since, error) -> None:
    path, db = _config_file(tmp_path)
    store.OptionStore(db).close()

    assert cli.main(["--config", path, "report", "--since", since]) == 2

    assert error in capsys.readouterr().err


def test_a_report_without_a_database_says_so_and_makes_none(tmp_path, capsys) -> None:
    path, db = _config_file(tmp_path)

    assert cli.main(["--config", path, "report"]) == 2

    assert "no options database at" in capsys.readouterr().err
    assert not db.exists()


# -- the shared-wallet banner and a refused step ------------------------------


def test_a_running_book_prints_the_banner_again_only_when_it_changes(
    tmp_path, monkeypatch, capsys, caplog
) -> None:
    cfg = _shared(tmp_path)
    asked = _engine(monkeypatch, _DOWN, _DOWN, _UP, _UP)
    seen: dict[str, Any] = {}

    with caplog.at_level(logging.INFO, logger="gui.services.options.__main__"):
        for _ in range(4):
            cli._shared_wallet_banner(cfg, None, seen)

    assert len(asked) == 4  # the engine is still asked every time
    out = capsys.readouterr().out
    assert out.count("share_trading_wallet is ON") == 2
    assert out.count("trading engine: not running (nothing there)") == 1
    assert out.count("trading engine: RUNNING (xop_trader.exe process 1)") == 1
    logged = [record for record in caplog.records if "share_trading_wallet" in record.getMessage()]
    assert [record.levelno for record in logged] == [logging.INFO, logging.WARNING]

    cli._shared_wallet_banner(cfg, None)  # status, which keeps nothing: every time
    cli._shared_wallet_banner(cfg, None)
    assert capsys.readouterr().out.count("share_trading_wallet is ON") == 2


def test_a_step_refused_for_the_engine_is_journaled(tmp_path, monkeypatch, capsys) -> None:
    cfg = _shared(tmp_path, dry_run=False)
    _engine(monkeypatch, _UP)
    journaled: list[tuple[str, str]] = []

    class Book:
        def journal(self, verdict: str, detail: str = "", market: Any = None, *, now: int) -> None:
            journaled.append((verdict, detail))

        def step(self) -> Any:
            raise AssertionError("a refused step is not taken")

    assert cli._guarded_step(cfg, Book(), None) is False

    assert journaled == [(ENGINE_REFUSED, "RUNNING (xop_trader.exe process 1)")]
    assert "refused, trading engine up: a live step" in capsys.readouterr().out


# -- status: can each unsold contract be cancelled now? ------------------------


def _cancel_lines(out: str) -> dict[str, str]:
    lines = out.splitlines()
    after = lines[lines.index("cancelling now:") + 1 :]
    return {line.split()[0]: line.split(None, 1)[1] for line in after}


def test_status_says_whether_each_unsold_contract_can_be_cancelled_now(
    tmp_path, monkeypatch, capsys
) -> None:
    world = World()
    maker, issuer = world.party(3), world.party(1)
    usd = world.issue_cat(issuer, 5)
    cfg = _config(tmp_path, usd)
    rpc = WalletRpc(world, maker, usd)
    monkeypatch.setattr(
        cli, "build", lambda _cfg: runtime.build(cfg, rpc=rpc, reader=world.sim, clock=world.now)
    )
    book_store = store.OptionStore(cfg.database_path)
    for launcher, slot, side, status in (
        (b"\x11" * 32, "C110@5", "call", store.OPEN),
        (b"\x22" * 32, "P90@5", "put", store.OPEN),
        (b"\x33" * 32, "C120@5", "call", store.SOLD),  # not ours to cancel
    ):
        call = side == "call"
        book_store.add_contract(
            launcher_id=launcher,
            slot=slot,
            side=side,
            underlying_asset=None if call else usd,
            underlying_amount=1,
            strike_asset=usd if call else None,
            strike_amount=1,
            expiration=_EXPIRY,
            creator_puzzle_hash=maker.puzzle_hash,
            bundle=b"x",
            deadline=9,
        )
        book_store.set_status(launcher, status)
    book_store.close()

    assert cli._status(cfg) == 0

    # A call's strike is paid in the quote asset, and there is none at the
    # options address; a put's is XCH, and there is XCH.
    assert _cancel_lines(capsys.readouterr().out) == {
        "C110@5": "needs a coin of the quote asset (any amount) at the options address; "
        "without it, it is clawed back after 2026-11-16 00:00 UTC",
        "P90@5": "possible",
    }

    world.send_cat(issuer, maker.puzzle_hash, usd, 1 * CAT)
    assert cli._status(cfg) == 0
    assert _cancel_lines(capsys.readouterr().out) == {"C110@5": "possible", "P90@5": "possible"}


def test_a_cancel_with_the_quote_asset_but_no_xch_may_still_need_a_fee(tmp_path) -> None:
    cfg = _config(tmp_path, USD)
    book_store = store.OptionStore(":memory:")
    book_store.add_contract(
        launcher_id=b"\x11" * 32,
        slot="C110@5",
        side="call",
        underlying_asset=None,
        underlying_amount=1,
        strike_asset=USD,
        strike_amount=1,
        expiration=_EXPIRY,
        creator_puzzle_hash=b"\x33" * 32,
        bundle=b"x",
        deadline=9,
    )

    class Coins:
        def __init__(self, xch: list, cats: list) -> None:
            self._xch, self._cats = xch, cats

        def xch_coins(self) -> list:
            return self._xch

        def cats(self, asset_id: bytes) -> list:
            return self._cats

    assert cli._cancel_funding(cfg, book_store, Coins([], ["usd"])) == []  # still minting

    book_store.set_status(b"\x11" * 32, store.OPEN)
    (_, line) = cli._cancel_funding(cfg, book_store, Coins([], ["usd"]))
    assert line.split(None, 1)[1] == "possible (plus XCH there if a fee is due)"
