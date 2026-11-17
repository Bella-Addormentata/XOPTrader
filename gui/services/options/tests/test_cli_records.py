"""What the command line leaves behind and says: the log file, the report
command, the shared-wallet banner, a refused step, and whether status can tell
that a cancel would be funded."""

from __future__ import annotations

import logging
import os
import re
import sqlite3
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
_EXPIRY = 4_102_444_800  # 2100-01-01 00:00 UTC: not expired while these tests run


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


def test_a_report_leaves_a_database_from_before_the_journals_as_it_was(tmp_path, capsys) -> None:
    # As v0.10.48 left it, with no quotes and no step journal. A book of that
    # version may still be using it: the report only reads.
    path, db = _config_file(tmp_path)
    book = store.OptionStore(db)
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
        deadline=int(time.time()) + 600,
        now=int(time.time()) - 600,
    )
    book.close()
    old = sqlite3.connect(db)
    old.executescript("DROP TABLE quotes; DROP TABLE step_log;")
    old.close()
    before = db.read_bytes()

    assert cli.main(["--config", path, "report", "--since", "all"]) == 0

    assert "C110@5" in capsys.readouterr().out
    assert db.read_bytes() == before


def test_a_report_on_a_file_that_is_no_database_says_so(tmp_path, capsys) -> None:
    path, db = _config_file(tmp_path)
    db.parent.mkdir(parents=True)
    db.write_bytes(b"not a database, just some bytes" * 20)

    assert cli.main(["--config", path, "report"]) == 2

    assert f"could not read {db}" in capsys.readouterr().err


def _files(root: Path) -> dict[Path, bytes]:
    return {file: file.read_bytes() for file in root.rglob("*") if file.is_file()}


@pytest.mark.parametrize(
    ("target", "clash"),
    [
        (lambda db, conf: db, "is the options database"),
        (lambda db, conf: db.parent / ".." / "data" / db.name, "is the options database"),
        (lambda db, conf: Path(f"{db}-journal"), "is the database's -journal file"),
        (lambda db, conf: Path(f"{db}.lock"), "is the database's .lock file"),
        (lambda db, conf: conf, "is the config file"),
    ],
    ids=["database", "another-spelling", "journal", "lock", "config"],
)
def test_a_report_does_not_write_its_csv_over_a_file_the_book_uses(
    tmp_path, capsys, target, clash
) -> None:
    # Writing the CSV would empty the file first: a slip of the tab key
    # could wipe the book's contracts and signed transactions.
    path, db = _config_file(tmp_path)
    store.OptionStore(db).close()
    before = _files(tmp_path)
    sheet = target(db, Path(path))

    assert cli.main(["--config", path, "report", "--since", "all", "--csv", str(sheet)]) == 2

    assert f"error: --csv {sheet} {clash}; the CSV needs a file of its own" in (
        capsys.readouterr().err
    )
    assert _files(tmp_path) == before


def test_a_report_does_not_write_its_csv_over_any_database(tmp_path, capsys) -> None:
    # A link to the book's database is the database; a copy of it is still
    # a database, and no place for a CSV.
    path, db = _config_file(tmp_path)
    store.OptionStore(db).close()
    copy = tmp_path / "book-copy.csv"
    copy.write_bytes(db.read_bytes())
    link = tmp_path / "book-link.csv"
    try:
        os.link(db, link)
    except OSError:
        pytest.skip("no hard links on this file system")
    before = _files(tmp_path)

    for sheet, clash in ((link, "is the options database"), (copy, "is an SQLite database")):
        assert cli.main(["--config", path, "report", "--csv", str(sheet)]) == 2
        assert f"--csv {sheet} {clash}" in capsys.readouterr().err
    assert _files(tmp_path) == before


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


def _minted(book_store: store.OptionStore, launcher: bytes, status: str) -> None:
    """Move a contract stored with a stand-in bundle to ``status``, its mint confirmed."""
    (mint,) = [row for row in book_store.broadcasts() if row.launcher_id == launcher]
    book_store.settle_broadcast(mint.id, "confirmed", contract_status=status)


class _Cat:
    """A CAT coin at the options address; only its coin is read here."""

    def __init__(self, coin: Any) -> None:
        self.coin = coin


def _coin(tag: int, amount: int = 1) -> Any:
    return sdk.Coin(bytes([tag]) * 32, b"\x55" * 32, amount)


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
        _minted(book_store, launcher, status)
    book_store.close()

    assert cli._status(cfg) == 0

    # A call's strike is paid in the quote asset, and there is none at the
    # options address; a put's is XCH, and there is XCH.
    assert _cancel_lines(capsys.readouterr().out) == {
        "C110@5": "needs a coin of the quote asset (any amount) at the options address; "
        "without it, it is clawed back after 2100-01-01 00:00 UTC",
        "P90@5": "possible",
    }

    world.send_cat(issuer, maker.puzzle_hash, usd, 1 * CAT)
    assert cli._status(cfg) == 0
    assert _cancel_lines(capsys.readouterr().out) == {"C110@5": "possible", "P90@5": "possible"}


class _Coins:
    """XCH and quote-asset coins at the options address."""

    def __init__(self, xch: list, cats: list) -> None:
        self._xch, self._cats = xch, cats

    def xch_coins(self) -> list:
        return self._xch

    def cats(self, asset_id: bytes) -> list:
        return self._cats


def _unsold(book_store: store.OptionStore, side: str = "call") -> None:
    call = side == "call"
    book_store.add_contract(
        launcher_id=b"\x11" * 32,
        slot="C110@5" if call else "P90@5",
        side=side,
        underlying_asset=None if call else USD,
        underlying_amount=1,
        strike_asset=USD if call else None,
        strike_amount=1,
        expiration=_EXPIRY,
        creator_puzzle_hash=b"\x33" * 32,
        bundle=b"x",
        deadline=9,
    )


_FEE_LIMITS = {"max_fee_mojos": 10_000_000}


@pytest.mark.parametrize(
    ("fee", "xch", "says"),
    [
        (0, 0, "possible"),
        ("auto", 0, "possible (plus XCH at the options address if a fee is due)"),
        (
            "auto",
            5,
            "possible if its fee, sized when it is sent, is within the 5 mojos of XCH "
            "at the options address",
        ),
        (1_000_000, 2_000_000, "possible"),
        (
            1_000_000,
            1,
            "needs 1000000 mojos of XCH at the options address for its fee; "
            "without it, it is clawed back after 2100-01-01 00:00 UTC",
        ),
    ],
)
def test_a_cancel_s_fee_is_checked_against_the_xch_there(tmp_path, fee, xch, says) -> None:
    cfg = _config(tmp_path, USD, fee_mojos=fee, limits=_FEE_LIMITS)
    book_store = store.OptionStore(":memory:")
    _unsold(book_store)
    coins = _Coins([_coin(2, xch)] if xch else [], [_Cat(_coin(1))])
    assert cli._cancel_funding(cfg, book_store, coins) == []  # still minting

    _minted(book_store, b"\x11" * 32, store.OPEN)

    (_, line) = cli._cancel_funding(cfg, book_store, coins)
    assert line.split(None, 1)[1] == says


def test_a_put_s_strike_and_fee_both_come_from_xch(tmp_path) -> None:
    cfg = _config(tmp_path, USD, fee_mojos=1_000_000, limits=_FEE_LIMITS)
    book_store = store.OptionStore(":memory:")
    _unsold(book_store, "put")
    _minted(book_store, b"\x11" * 32, store.OPEN)

    (_, line) = cli._cancel_funding(cfg, book_store, _Coins([_coin(2, 1)], []))
    assert line.split(None, 1)[1].startswith(
        "needs 1000000 mojos of XCH at the options address for its fee;"
    )
    (_, line) = cli._cancel_funding(cfg, book_store, _Coins([_coin(2, 1_000_000)], []))
    assert line.split(None, 1)[1] == "possible"


def test_an_expired_contract_is_not_offered_a_cancel(tmp_path) -> None:
    # The chain refuses a cancel after expiry; a stopped book has not yet
    # noticed, so the contract is still on record as open.
    cfg = _config(tmp_path, USD)
    book_store = store.OptionStore(":memory:")
    _unsold(book_store)
    _minted(book_store, b"\x11" * 32, store.OPEN)

    coins = _Coins([_coin(2, 10**9)], [_Cat(_coin(1))])
    (_, line) = cli._cancel_funding(cfg, book_store, coins, now=_EXPIRY + 60)

    assert line.split(None, 1)[1] == (
        "expired at 2100-01-01 00:00 UTC, so it can only be clawed back; a live step does that"
    )


def test_with_fund_from_wallet_a_cancel_may_start_from_anywhere_in_the_wallet(tmp_path) -> None:
    wallet = {"port": 9356, "cert_path": "c", "key_path": "k", "fund_from_wallet": True}
    cfg = _config(tmp_path, USD, wallet=wallet)
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
    _minted(book_store, b"\x11" * 32, store.OPEN)

    class Wallet:
        """Nothing at the options address; ``rest`` is what the rest of the wallet holds."""

        def __init__(self, rest: list) -> None:
            self._rest = rest

        def xch_coins(self) -> list:
            return []

        def cats(self, asset_id: bytes) -> list:
            return []

        def wallet_xch_coins(self) -> list:
            return self._rest

        def wallet_cat_coins(self, asset_id: bytes) -> list:
            return self._rest

    (_, line) = cli._cancel_funding(cfg, book_store, Wallet([_coin(2)]))
    assert line.split(None, 1)[1] == "possible"
    (_, line) = cli._cancel_funding(cfg, book_store, Wallet([]))
    assert "needs a coin of the quote asset (any amount) in the wallet;" in line


def test_a_coin_a_waiting_transaction_spends_cannot_start_a_cancel(tmp_path) -> None:
    # The cancel leaves such a coin alone, so status must not count it either.
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
    _minted(book_store, b"\x11" * 32, store.OPEN)
    seed, xch = _coin(1), _coin(2, 10**9)

    class Coins:
        def xch_coins(self) -> list:
            return [xch]

        def cats(self, asset_id: bytes) -> list:
            return [_Cat(seed)]

    (_, line) = cli._cancel_funding(cfg, book_store, Coins())
    assert line.split(None, 1)[1] == "possible"

    # Another contract's mint, still waiting, spends the only quote-asset coin.
    nil = bytes([0x80])
    book_store.add_contract(
        launcher_id=b"\x22" * 32,
        slot="C120@5",
        side="call",
        underlying_asset=None,
        underlying_amount=1,
        strike_asset=USD,
        strike_amount=1,
        expiration=_EXPIRY,
        creator_puzzle_hash=b"\x33" * 32,
        bundle=cli.pack_bundle([sdk.CoinSpend(seed, nil, nil)], bytes([0xC0]) + bytes(95)),
        deadline=9,
    )

    (_, line) = cli._cancel_funding(cfg, book_store, Coins())
    assert line.split(None, 1)[1].startswith(
        "needs a coin of the quote asset (any amount) at the options address;"
    )
