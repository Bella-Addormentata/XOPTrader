"""Run the options book from a terminal.

    python -m gui.services.options --config config.yaml status
    python -m gui.services.options --config config.yaml step
    python -m gui.services.options --config config.yaml run --interval 60
    python -m gui.services.options --config config.yaml withdraw-all
    python -m gui.services.options --config config.yaml monitor --interval 300
    python -m gui.services.options --config config.yaml live-test
    python -m gui.services.options --config config.yaml report --since 24h

THE LOG FILE. Every command but ``status`` and ``report`` also logs to a file
beside the book's database (``options_jobs.log`` for ``options_jobs.db``),
rotated at 2 MB with three old files kept, with the time on every line. It
holds what the terminal shows and more: the market each step, every action and
problem, and engine-guard changes. ``--log-file`` puts it elsewhere and
``--no-log-file`` turns it off. The terminal shows each problem once, as a
``note:`` line, and the shared-wallet banner only when what it says changes.

``status``, ``monitor`` and a dry-run ``step`` sign nothing. ``live-test`` is the
guided first run against a real wallet: it mints one dust-sized contract, offers
it, withdraws the offer and cancels the contract, pausing before each spend to
show exactly what the bundle does. Its contract and every bundle it signs are
written to the book's database before they are sent, so a test that is
interrupted, times out or has its offer taken is picked up by running
``live-test`` again. See ``docs/options-live-test.md``.
"""

from __future__ import annotations

import argparse
import dataclasses
import logging
import os
import sqlite3
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from gui.services.options import builder, chain, config, engine_guard, fees, safety, store
from gui.services.options import report as reporting
from gui.services.options.reference_wallet import ReferenceWallet
from gui.services.options.runtime import (
    _ADDITIONAL_DATA,
    LOG_BACKUPS,
    build,
    chain_reader,
    chain_source,
    log_handler,
    log_path,
    shared_wallet_guard,
    wallet_rpc,
)
from gui.services.options.service import (
    ENGINE_REFUSED,
    REPORTED,
    pack_bundle,
    reserved_coin_ids,
    unpack_bundle,
)

log = logging.getLogger(__name__)

#: Commands that only look: they leave the log file to the one that acts.
_NO_LOG_FILE = ("status", "report")

#: The slot a live-test contract is stored under. It is in no grid, so a live
#: book that finds one still open retires it, and claws it back once expired.
LIVE_TEST_SLOT = "live-test"

_POLL_SECONDS = 15
_REPUSH_EVERY_POLLS = 8  # two minutes: the first push may never have reached a node
# With ``fee_mojos: auto`` the test pays one flat fee per spend, sized for the
# costliest of them (the cancel, about 165 million), so its sums stay simple.
_LIVE_TEST_COST = 170_000_000


class _NotReported(logging.Filter):
    """Lets through only records the printed step report does not already show."""

    def filter(self, record: logging.LogRecord) -> bool:
        return not getattr(record, "options_report", False)


def _log_to_console(verbose: bool) -> logging.Handler:
    """Warnings (everything with ``--verbose``) to the terminal, as before the log file."""
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    console = logging.StreamHandler()
    console.setLevel(logging.INFO if verbose else logging.WARNING)
    console.setFormatter(logging.Formatter(logging.BASIC_FORMAT))
    console.addFilter(_NotReported())
    root.addHandler(console)
    return console


def default_log_path(cfg: config.OptionsConfig) -> Path | None:
    """Beside the book's database, named after it; None for an in-memory one."""
    return log_path(cfg)


def _log_to_file(path: Path) -> logging.Handler:
    """Everything from info up, with the time on every line, rotated as the GUI's log is."""
    handler = log_handler(path)
    logging.getLogger().addHandler(handler)
    return handler


def _load(path: str) -> config.OptionsConfig:
    # With secrets.yaml from the same folder laid over it, as the GUI and the
    # engine read it: that is where certificate paths and API keys are kept.
    from gui.services.config_split import load_merged

    root = load_merged(Path(path))
    parsed = config.parse(root)
    if parsed is None:
        raise SystemExit(f"{path}: options are not enabled (options.enabled: true to turn on)")
    # Read beside config.yaml, as the app's Options page and its engine check do.
    return config.resolve_database_path(parsed, path)


def _print_report(report: Any) -> None:
    if report.market is not None:
        view = report.market
        book = (
            "no book" if view.bid is None or view.ask is None else f"{view.bid:.4f}/{view.ask:.4f}"
        )
        print(f"market: spot {view.spot:.4f}  sigma {view.sigma:.2f}  dexie {book}")
    if report.dry_run:
        print("DRY RUN: nothing is signed, sent or listed")
    for action in report.actions:
        print(f"  did: {action}")
    for problem in report.problems:
        print(f"  note: {problem}")
    if not report.actions and not report.problems:
        print("  nothing to do")
    if report.market is not None:
        print(f"net delta of sold contracts: {report.net_delta_xch:+.2f} XCH")
    looked = f" ({report.chain_looked})" if report.chain_looked else ""
    print(f"chain requests this step: {report.chain_requests}{looked}")


def _shared_wallet_banner(
    cfg: config.OptionsConfig,
    book_store: store.OptionStore | None,
    seen: dict[str, Any] | None = None,
) -> engine_guard.EngineState | None:
    """With ``share_trading_wallet`` on: say so, loudly, and whether the engine is up.

    None when the opt-in is off. Printed by ``status`` and before every step.
    With ``seen`` (``run`` keeps one across its steps) it is printed only when
    what it says has changed since it was last printed; the engine is still
    asked every time.
    """
    if not cfg.wallet.share_trading_wallet:
        return None
    beside = cfg.wallet.engine_beside_open_contracts
    state = engine_guard.engine_state(cfg.wallet.engine_metrics_url)
    rule = (
        "The engine may run while contracts sit open (engine_beside_open_contracts), but "
        "must be STOPPED while this book acts."
        if beside
        else "The engine must stay STOPPED while any contract is open."
    )
    lines = [
        "WARNING: options.wallet.share_trading_wallet is ON. This book signs from the "
        f"trading engine's wallet ({cfg.wallet.host}:{cfg.wallet.port}, key "
        f"{cfg.wallet.fingerprint}). {rule}",
        f"  trading engine: {state.describe()}",
    ]
    open_contracts = book_store.contracts(store.ACTIVE) if book_store is not None else []
    if open_contracts and not state.clear:
        locked = sum(
            item.underlying_amount for item in open_contracts if item.underlying_asset is None
        )
        if beside:
            lines.append(
                f"  note: {len(open_contracts)} contract(s) open with "
                f"{locked / config.XCH_MOJOS:g} XCH locked beside the engine: their collateral "
                "is locked in the contracts; a live step waits for the engine to stop, and "
                "then pays from the rest of the wallet what the options address lacks"
            )
        else:
            lines.append(
                f"  WARNING: {len(open_contracts)} contract(s) open with "
                f"{locked / config.XCH_MOJOS:g} XCH locked while the engine may be running: its "
                "balances include collateral and premiums it must not count, and its offers can "
                "spend coins at the options address"
            )
    if seen is not None and seen.get("banner") == lines:
        return state
    if seen is not None:
        seen["banner"] = lines
    for line in lines:
        print(line)
        log.log(
            logging.INFO if state.clear else logging.WARNING,
            "[options] %s",
            line.strip(),
            extra=REPORTED,
        )
    return state


def _engine_refusal(
    state: engine_guard.EngineState | None,
    what: str,
    cfg: config.OptionsConfig | None = None,
) -> str | None:
    """Why ``what``, which signs, must not run now; None if it may."""
    if state is None or state.clear:
        return None
    if cfg is not None and cfg.wallet.engine_beside_open_contracts:
        how = "Stop the engine while the book acts (it may run again once the book is done)"
    else:
        how = "Stop the engine and keep it stopped while contracts are open"
    return (
        f"refused, trading engine up: {what} would sign from the wallet the engine trades "
        f"through, and the engine is {state.describe()}. {how}, then try again."
    )


def _learned_rate(book_store: store.OptionStore) -> float:
    """The fee rate the book has learned from transactions that ran out, as last stored."""
    try:
        return float(book_store.recall("fee_rate") or 0.0)
    except ValueError:
        return 0.0


def _status(cfg: config.OptionsConfig) -> int:
    book, book_store, coins = build(cfg)
    print(f"network {cfg.network}   dry_run {cfg.dry_run}")
    _shared_wallet_banner(cfg, book_store)
    key = (
        f"logged in to {cfg.wallet.fingerprint}"
        if cfg.wallet.fingerprint is not None
        else "no fingerprint set, so whichever key it has open"
    )
    not_ready = book.wallet_not_ready()
    print(f"wallet: {not_ready or f'synced ({key})'}")
    print(f"chain is read from {chain_source(cfg)}")
    if cfg.fee_auto:
        print(
            f"fees: auto, at most {cfg.limits.max_fee_mojos} mojos a transaction "
            f"(rate learned from failures: {_learned_rate(book_store):g} mojos per cost)"
        )
    else:
        print(f"fees: {cfg.fee_mojos} mojos a transaction")
    if cfg.splash.enabled:
        live = book_store.offers(status="live")
        taken = sum(offer.splash_at is not None for offer in live)
        print(
            f"splash: new offers are handed to the daemon at {cfg.splash.submit_url} "
            f"({taken} of {len(live)} live offers taken by it)"
        )
    else:
        print("splash: off")
    if not_ready is not None and not coins.known_address():
        # Asking now could take, and keep, the address of whichever key the
        # wallet happens to have open. The first step asks, once it has the
        # right key selected.
        print("options address: not chosen yet; the first step takes it from the wallet")
    else:
        print(f"options address {coins.address()}")
        xch = sum(int(coin.amount) for coin in coins.xch_coins())
        quote = sum(int(cat.coin.amount) for cat in coins.cats(cfg.quote_asset_id))
        print(
            f"  spendable there: {xch / config.XCH_MOJOS:g} XCH, "
            f"{quote / cfg.quote_unit_mojos:g} quote"
        )
        if cfg.wallet.fund_from_wallet:
            here = coins.puzzle_hash()
            rest = sum(
                int(coin.amount)
                for coin in coins.wallet_xch_coins()
                if bytes(coin.puzzle_hash) != here
            )
            print(
                f"  spendable elsewhere in the wallet: {rest / config.XCH_MOJOS:g} XCH "
                "(fund_from_wallet: what the options address is short of comes from here; "
                "change comes back to the options address)"
            )
    contracts = book_store.contracts()
    print(f"{len(contracts)} contract(s)")
    for item in contracts:
        live = len(book_store.offers(item.launcher_id, "live"))
        print(f"  {item.slot:<22} {item.status:<10} {item.launcher_id.hex()}  live offers: {live}")
    for row in book_store.unresolved():
        print(f"  waiting: {row.kind} of {row.launcher_id.hex()[:10]} ({row.status})")
    if not_ready is None or coins.known_address():
        for line in _cancel_funding(cfg, book_store, coins):
            print(line)
    return 0


def _cancel_funding(
    cfg: config.OptionsConfig,
    book_store: store.OptionStore,
    coins: Any,
    now: float | None = None,
) -> list[str]:
    """Whether each unsold contract could be cancelled now, and what is missing if not.

    A cancel pays the strike to ourselves inside the same bundle, so it needs
    one coin of the strike asset to start from (any amount; it comes back in
    full), and XCH for its fee: at the options address, or with
    ``fund_from_wallet`` anywhere in the wallet. A fixed ``fee_mojos`` is
    checked against that XCH; with ``fee_mojos: auto`` the fee is sized only
    when the cancel is sent. An expired contract cannot be cancelled at all:
    it is clawed back, which needs neither. On the first live run the
    wind-down found this out from the cancel's error. Coins a transaction
    still waiting already spends do not count, as they do not for the cancel
    itself.
    """
    unsold = book_store.contracts((store.OPEN,))
    if not unsold:
        return []
    now = time.time() if now is None else now
    anywhere = cfg.wallet.fund_from_wallet
    where = "in the wallet" if anywhere else "at the options address"
    reserved = reserved_coin_ids(book_store)

    def free(listed: list[Any]) -> list[Any]:
        return [coin for coin in listed if bytes(coin.coin_id()) not in reserved]

    xch = sum(
        int(coin.amount)
        for coin in free(coins.wallet_xch_coins() if anywhere else coins.xch_coins())
    )
    fee = None if cfg.fee_auto else cfg.fee_mojos  # None: sized when the cancel is sent
    lines = ["cancelling now:"]
    for item in unsold:
        when = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(item.expiration))
        if item.expiration <= now:
            lines.append(
                f"  {item.slot:<22} expired at {when}, so it can only be clawed back; "
                "a live step does that"
            )
            continue
        missing = []
        if item.strike_asset is None:
            # The strike is XCH: one XCH coin starts it, and the fee comes from the same XCH.
            if xch < max(fee or 0, 1):
                missing.append(
                    f"{fee} mojos of XCH {where} for its fee" if fee else f"an XCH coin {where}"
                )
        else:
            asset = item.strike_asset
            listed = (
                coins.wallet_cat_coins(asset)
                if anywhere
                else [cat.coin for cat in coins.cats(asset)]
            )
            if not free(listed):
                name = "the quote asset" if asset == cfg.quote_asset_id else "its CAT"
                missing.append(f"a coin of {name} (any amount) {where}")
            if fee and xch < fee:
                missing.append(f"{fee} mojos of XCH {where} for its fee")
        if missing:
            them = "them" if len(missing) > 1 else "it"
            lines.append(
                f"  {item.slot:<22} needs {' and '.join(missing)}; "
                f"without {them}, it is clawed back after {when}"
            )
        elif fee is not None:
            lines.append(f"  {item.slot:<22} possible")
        elif xch:
            lines.append(
                f"  {item.slot:<22} possible if its fee, sized when it is sent, is within "
                f"the {xch} mojos of XCH {where}"
            )
        else:
            lines.append(f"  {item.slot:<22} possible (plus XCH {where} if a fee is due)")
    return lines


def _guarded_step(
    cfg: config.OptionsConfig,
    book: Any,
    book_store: store.OptionStore,
    seen: dict[str, Any] | None = None,
) -> bool:
    """One step, unless the shared-wallet guard forbids it. False if refused."""
    state = _shared_wallet_banner(cfg, book_store, seen)
    # A dry-run step signs nothing, so only a live one needs the engine down.
    refusal = None if cfg.dry_run else _engine_refusal(state, "a live step", cfg)
    if refusal is not None:
        print(f"  note: {refusal}")
        log.warning("[options] %s", refusal, extra=REPORTED)
        book.journal(ENGINE_REFUSED, state.describe() if state else "", now=int(time.time()))
        return False
    _print_report(book.step())
    return True


def _step(cfg: config.OptionsConfig) -> int:
    book, book_store, _ = build(cfg)
    return 0 if _guarded_step(cfg, book, book_store) else 3


def _run(cfg: config.OptionsConfig, interval: float) -> int:
    book, book_store, _ = build(cfg)
    book.step_interval = interval
    print(f"stepping every {interval:g} s; Ctrl-C to stop (offers already out expire on their own)")
    log.info("[options] stepping every %g s (dry_run %s)", interval, cfg.dry_run)
    seen: dict[str, Any] = {}  # what the shared-wallet banner last said
    try:
        while True:
            # A log of a long run is read by time; nothing in the report says when.
            print(f"--- step at {time.strftime('%Y-%m-%d %H:%M:%S %z')}", flush=True)
            try:
                _guarded_step(cfg, book, book_store, seen)
            except Exception as exc:  # noqa: BLE001 - keep the loop alive; the next step retries
                log.exception("step failed: %s", exc)
            time.sleep(interval)
    except KeyboardInterrupt:
        print("stopped")
        log.info("[options] stopped")
        return 0


def _monitor(cfg: config.OptionsConfig, interval: float) -> int:
    """Read-only: record what the desk lists and what becomes of it."""
    from gui.services.options import desk, market, monitor, store

    if cfg.network != config.MAINNET:
        raise SystemExit("the desk lists mainnet options only")
    watcher = monitor.DeskMonitor(
        desk_client=desk.DeskClient(),
        chain_reader=chain_reader(cfg),
        market=market.MarketSource(
            cfg.quote_asset_id,
            hedge_size_xch=cfg.market.hedge_size_xch,
            coingecko_api_key=cfg.coingecko_api_key,
        ).read,
        book_store=store.OptionStore(cfg.database_path),
        quote_asset_id=cfg.quote_asset_id,
        quote_unit_mojos=cfg.quote_unit_mojos,
        rate=cfg.pricing.rate,
        max_spot_age_seconds=cfg.market.max_age_seconds,
    )

    def number(value: float | None) -> str:
        return "     -" if value is None else f"{value:6.2f}"

    while True:
        report = watcher.scan()
        print(
            f"{time.strftime('%H:%M:%S')} listed {report.listed}, new {len(report.new)}, "
            f"closed {len(report.closed)}, chain requests {report.chain_requests}"
        )
        if report.market_error:
            print(f"  no market view: {report.market_error}")
        for item in report.new:
            terms = (
                "other assets"
                if item.side is None
                else (f"{item.side} {item.size_xch:g} XCH @ {item.strike:.3f}")
            )
            print(
                f"  new {item.offer_id[:8]} {terms:<26} ask {number(item.ask)}  "
                f"site {number(item.site_premium)}  ours {number(item.our_value)}  "
                f"edge after hedge {number(item.edge)}"
            )
        for offer_id, outcome in report.closed.items():
            print(f"  {offer_id[:8]} {outcome}")
        for problem in report.problems:
            print(f"  note: {problem}")
        if interval <= 0:
            return 0
        try:
            time.sleep(interval)
        except KeyboardInterrupt:
            print("stopped")
            return 0


#: Files that SQLite and the book keep beside the database.
_DATABASE_COMPANIONS = ("-journal", "-wal", "-shm", ".lock")
_SQLITE_HEADER = b"SQLite format 3\x00"


def _same_file(a: Path, b: Path) -> bool:
    """Whether two paths name one file, through a link or another spelling."""
    try:
        return os.path.samefile(a, b)
    except OSError:  # one of them is not there (yet)
        pass
    try:
        return os.path.normcase(str(a.resolve())) == os.path.normcase(str(b.resolve()))
    except (OSError, RuntimeError):  # a link loop, say: writing there fails on its own
        return False


def _book_files(
    cfg: config.OptionsConfig, config_path: str | None, log_file: Path | None
) -> list[tuple[Path, str]]:
    """The files the book keeps, each with what a refusal calls it."""
    files: list[tuple[Path, str]] = []
    if cfg.database_path != ":memory:":
        database = Path(cfg.database_path)
        files.append((database, "the options database"))
        files += [
            (Path(f"{database}{suffix}"), f"the database's {suffix} file")
            for suffix in _DATABASE_COMPANIONS
        ]
    if config_path is not None:
        files.append((Path(config_path), "the config file"))
    if log_file is not None:
        files.append((log_file, "the book's log file"))
        files += [
            (Path(f"{log_file}.{n}"), "one of the book's older log files")
            for n in range(1, LOG_BACKUPS + 1)
        ]
    return files


def _clash(target: Path, files: list[tuple[Path, str]]) -> str | None:
    """What ``target`` is, when nothing may be written to it: one of ``files``
    (through a link or another spelling too), or any SQLite database (a copy
    of the book's, say)."""
    for path, name in files:
        if _same_file(target, path):
            return f"is {name}"
    try:
        with open(target, "rb") as handle:
            if handle.read(len(_SQLITE_HEADER)) == _SQLITE_HEADER:
                return "is an SQLite database"
    except OSError:
        pass  # nothing there yet, or nothing readable: the write says what is wrong
    return None


def _report(
    cfg: config.OptionsConfig,
    since: str,
    csv_path: str | None,
    config_path: str | None = None,
    log_file: Path | None = None,
) -> int:
    """Read-only: what the book did since ``since``."""
    now = time.time()
    try:
        start = reporting.parse_since(since, now)
    except reporting.ReportError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if cfg.database_path != ":memory:" and not Path(cfg.database_path).is_file():
        # Said plainly, rather than in SQLite's words.
        print(f"error: no options database at {cfg.database_path}", file=sys.stderr)
        return 2
    if start > now:
        print(f"error: --since {since!r} is in the future", file=sys.stderr)
        return 2
    # Writing the CSV empties its file first: never one the book keeps.
    clash = _clash(Path(csv_path), _book_files(cfg, config_path, log_file)) if csv_path else None
    if clash is not None:
        print(f"error: --csv {csv_path} {clash}; the CSV needs a file of its own", file=sys.stderr)
        return 2
    try:
        # A copy: the file is only read, even when it is from an older version.
        book_store = store.OptionStore.snapshot(cfg.database_path)
    except sqlite3.Error as exc:
        print(f"error: could not read {cfg.database_path}: {exc}", file=sys.stderr)
        return 2
    try:
        if start == 0:  # all of it: from the first thing the book recorded
            start = min(book_store.first_record_at() or int(now), int(now))
        summary = reporting.summarize(book_store, cfg.quote_unit_mojos, start, int(now))
    finally:
        book_store.close()
    print(reporting.format_text(summary))
    if csv_path:
        written = reporting.write_csv(summary, csv_path)
        print(f"\n{written} contract row(s) written to {csv_path}")
    return 0


def _withdraw_all(cfg: config.OptionsConfig) -> int:
    book, book_store, _ = build(cfg)
    state = _shared_wallet_banner(cfg, book_store)
    refusal = None if cfg.dry_run else _engine_refusal(state, "withdraw-all", cfg)
    if refusal is not None:
        print(f"  note: {refusal}")
        return 3
    _print_report(book.withdraw_all())
    return 0


# ---------------------------------------------------------------------------
# live-test
# ---------------------------------------------------------------------------


def _confirm(prompt: str, assume_yes: bool) -> None:
    if assume_yes:
        print(f"{prompt} yes (--yes)")
        return
    if input(f"{prompt} [type yes] ").strip().lower() != "yes":
        raise SystemExit("stopped; nothing further was sent")


@dataclasses.dataclass
class _LiveTest:
    """What every stage of the guided test needs."""

    cfg: config.OptionsConfig
    wallet: ReferenceWallet
    reader: Any
    book: Any
    book_store: store.OptionStore
    coins: Any
    address: bytes
    key: Any
    custody: builder.Custody
    assume_yes: bool
    timeout: float
    fee: int = 0  # paid by each of the test's spends

    @staticmethod
    def now() -> int:
        return int(time.time())

    def deadline(self) -> int:
        return self.now() + self.cfg.confirm_timeout_seconds

    def valid_until(self) -> int | None:
        """The time a spend signed now stops being valid, as the book sets it.

        Unconfirmed by then it never can be, so it is given up (and teaches the
        fee rate) instead of holding its coins for ever. None when the chain's
        time cannot be read, since such a spend could never be seen to lapse.
        """
        return self.deadline() if self.reader.tells_time else None

    def free_xch(self) -> list[Any]:
        """The address's XCH coins that no transaction still waiting already spends.

        The wallet lists a coin as spendable until its spend confirms, and it
        does not know about bundles pushed through it. Funding the test from a
        coin a waiting mint of the book's is counting on would knock that mint
        out.
        """
        reserved = reserved_coin_ids(self.book_store)
        return [coin for coin in self.coins.xch_coins() if bytes(coin.coin_id()) not in reserved]

    def settle(self) -> None:
        """Bring the stored book up to date with the chain. Sends nothing."""
        for problem in self.book.refresh().problems:
            print(f"  note: {problem}")

    def wait(self, coin_spends: list[Any], signature: bytes, what: str) -> bool:
        """Wait for a pushed bundle. True once it confirms, False if it was overtaken."""
        deadline = time.time() + self.timeout
        print(f"  waiting for the {what} to confirm", end="", flush=True)
        polls = 0
        while time.time() < deadline:
            self.reader.begin()  # each poll reads afresh: one request for the bundle's coins
            try:
                status = chain.bundle_status(self.reader, coin_spends)
            except chain.ChainError:
                status = chain.BundleStatus.PENDING  # a failed lookup decides nothing
            if status is chain.BundleStatus.CONFIRMED:
                print(" confirmed")
                return True
            if status is chain.BundleStatus.CONFLICT:
                print(" overtaken")
                return False
            print(".", end="", flush=True)
            time.sleep(_POLL_SECONDS)
            polls += 1
            if polls % _REPUSH_EVERY_POLLS == 0:
                self.wallet.push(coin_spends, signature)
        raise SystemExit(
            f"\nthe {what} did not confirm in {self.timeout:g} s. The wallet reports no verdict "
            "on a broadcast; check its full-node connection and the mempool. The bundle is on "
            "record: run live-test again to pick it up."
        )

    def send(
        self,
        coin_spends: list[Any],
        expected: safety.Expected,
        lock: bytes,
        what: str,
        record: Callable[[bytes], Any],
    ) -> bool:
        """Show a bundle, ask, sign, write it down, then push it and wait.

        ``record`` stores the signed bundle; it runs before the push, so
        nothing is ever on its way to the chain that the database does not
        know. True once the bundle confirms, False if it was overtaken.
        """
        effect = safety.analyse(coin_spends, ours=[self.address], locks=[lock])
        print(f"  the {what} does this: {effect.describe()}")
        safety.require(effect, expected)
        _confirm(f"  sign and send the {what}?", self.assume_yes)
        # Asked again at every spend: the engine may have been started since.
        refusal = _engine_refusal(_shared_wallet_banner(self.cfg, None), f"the {what}", self.cfg)
        if refusal is not None:
            raise SystemExit(f"{refusal} Nothing was signed; run live-test again to carry on.")
        signature = self.wallet.sign(coin_spends)
        record(pack_bundle(coin_spends, signature))
        self.wallet.push(coin_spends, signature)
        confirmed = self.wait(coin_spends, signature, what)
        self.settle()
        return confirmed


def _cancel_stage(test: _LiveTest, contract: store.Contract) -> None:
    loaded = chain.load_option(test.reader, contract.launcher_id, now_seconds=test.now())
    fee = test.fee
    amount = contract.underlying_amount
    valid_until = test.valid_until()
    cancel = builder.build_cancel(
        loaded.option,
        loaded.underlying,
        loaded.locked,
        builder.Funding(test.address, xch=test.free_xch()),
        test.custody,
        fee=fee,
        valid_until_seconds=valid_until,
    )

    def record(bundle: bytes) -> None:
        test.book_store.add_broadcast(
            launcher_id=contract.launcher_id,
            kind=store.CANCEL,
            bundle=bundle,
            deadline=test.deadline(),
            contract_status=store.CLOSING,
            valid_until=valid_until,
            fee=fee,
            now=test.now(),
        )

    confirmed = test.send(
        cancel,
        safety.Expected(
            wallet={None: amount + builder.OPTION_COIN_AMOUNT - fee},
            locked={None: -amount},
            max_fee=fee,
        ),
        bytes(loaded.underlying.puzzle_hash()),
        "cancel",
        record,
    )
    if not confirmed:
        raise SystemExit("the cancel was overtaken by another spend; run live-test again")


def _clawback_stage(test: _LiveTest, contract: store.Contract) -> None:
    loaded = chain.load_option(test.reader, contract.launcher_id, now_seconds=test.now())
    fee = test.fee
    amount = contract.underlying_amount
    valid_until = test.valid_until()
    # XCH collateral pays its own fee when it can. The test's dust collateral
    # is smaller than a fee under pressure, and then other coins make it up.
    extra = test.free_xch() if fee >= amount else []
    clawback = builder.build_clawback(
        loaded.underlying,
        loaded.locked,
        test.key,
        builder.Funding(change_puzzle_hash=test.address, xch=extra),
        test.custody,
        fee=fee,
        valid_until_seconds=valid_until,
    )

    def record(bundle: bytes) -> None:
        test.book_store.add_broadcast(
            launcher_id=contract.launcher_id,
            kind=store.CLAWBACK,
            bundle=bundle,
            deadline=test.deadline(),
            contract_status=store.CLOSING,
            valid_until=valid_until,
            fee=fee,
            now=test.now(),
        )

    confirmed = test.send(
        clawback,
        safety.Expected(wallet={None: amount - fee}, locked={None: -amount}, max_fee=fee),
        bytes(loaded.underlying.puzzle_hash()),
        "clawback",
        record,
    )
    if not confirmed:
        raise SystemExit("the clawback was overtaken by another spend; run live-test again")


def _finish_live_test(test: _LiveTest, contract: store.Contract) -> None:
    """Pick up a test contract an earlier run left behind, from what the chain shows."""
    launcher_id = contract.launcher_id
    print(f"an earlier live test left contract {launcher_id.hex()} unfinished; picking it up")
    test.settle()
    expired = test.now() >= contract.expiration
    # Whatever of its own is still in flight was approved when it was signed:
    # send it again and wait. A cancel past expiry is the exception. It can
    # never confirm, and the clawback below takes its place.
    for row in test.book_store.unresolved():
        if row.launcher_id != launcher_id or (row.kind == store.CANCEL and expired):
            continue
        if row.valid_until is not None and test.now() >= row.valid_until:
            # Settled above if the chain already shows it out of time; if not,
            # it soon will, and pushing it again cannot help.
            raise SystemExit(
                f"  its {row.kind} ran out of time unconfirmed and can no longer confirm. It is "
                "given up once the chain is 32 blocks past its deadline, about a quarter of an "
                "hour after it: run live-test again then, and it is made afresh at the fee "
                "rate that failure taught."
            )
        coin_spends, signature = unpack_bundle(row.bundle)
        print(f"  its {row.kind} was signed but has not confirmed; sending it again")
        test.wallet.push(coin_spends, signature)
        test.wait(coin_spends, signature, row.kind)
        test.settle()

    current = test.book_store.contract(launcher_id)
    if current is None or current.status not in store.ACTIVE:
        status = "gone" if current is None else current.status
        print(f"  it is {status}; it locks nothing any more")
        return
    loaded = chain.load_option(test.reader, launcher_id, now_seconds=test.now())
    if loaded.status is chain.OptionStatus.EXPIRED:
        print("  it has expired with its collateral still locked; clawing it back")
        _clawback_stage(test, current)
    elif loaded.locked is not None and loaded.owner_puzzle_hash == test.address:
        print("  it is live and still ours; cancelling it to get the collateral back")
        _cancel_stage(test, current)
    elif loaded.locked is not None:
        when = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(current.expiration))
        print(f"  it was sold. Its collateral can be clawed back after {when}, unless the")
        print("  buyer exercises it first: run live-test again then.")
        return
    final = test.book_store.contract(launcher_id)
    print(f"  it is now {'gone' if final is None else final.status}")


def _live_test(
    cfg: config.OptionsConfig, amount_xch: float, assume_yes: bool, timeout: float
) -> int:
    """Mint, offer, withdraw and cancel one dust contract against the real wallet.

    The contract locks XCH and is struck in XCH, so the whole test needs nothing
    but a little XCH at the options address and returns all of it but the fees.
    """
    rpc = wallet_rpc(cfg)
    reader = chain_reader(cfg)
    # Checked for a running engine at every sign and push when shared,
    # the offer's signature included, not only before each spend's prompt.
    wallet = shared_wallet_guard(
        cfg,
        ReferenceWallet(
            rpc,
            agg_sig_me_additional_data=_ADDITIONAL_DATA[cfg.network],
            address_prefix=cfg.address_prefix,
            fingerprint=cfg.wallet.fingerprint,
        ),
    )
    why = wallet.not_ready()
    if why is not None:
        raise SystemExit(f"{why}; run the test again once it is ready")
    book, book_store, coins = build(cfg, rpc=rpc, reader=reader, clock=time.time)
    refusal = _engine_refusal(_shared_wallet_banner(cfg, book_store), "live-test", cfg)
    if refusal is not None:
        raise SystemExit(refusal)
    address = coins.puzzle_hash()
    print(f"network {cfg.network}; options address {coins.address()}")
    print(f"chain is read from {chain_source(cfg)}")
    print("1. asking the wallet for the address's public key")
    key = wallet.synthetic_key(address)
    custody = builder.Custody()
    custody.add_key(key)
    fee = cfg.fee_mojos
    if cfg.fee_auto:
        estimate = wallet.fee_rate()
        rate = max(fees.pressure_rate(estimate), _learned_rate(book_store))
        fee = fees.fee_for(_LIVE_TEST_COST, rate, cfg.limits.max_fee_mojos)
        said = (
            "no estimate from the wallet"
            if estimate is None
            else f"the wallet estimates {estimate}"
        )
        print(
            f"fee per spend: {fee} mojos (auto: {said}, paying {rate:g} mojos per cost, "
            f"capped at {cfg.limits.max_fee_mojos})"
        )
    test = _LiveTest(
        cfg=cfg,
        wallet=wallet,
        reader=book.chain,
        book=book,
        book_store=book_store,
        coins=coins,
        address=address,
        key=key,
        custody=custody,
        assume_yes=assume_yes,
        timeout=timeout,
        fee=fee,
    )
    try:
        # Held throughout: a book stepping over the same database must not act
        # on the test contract, or spend its coins, half way through.
        with book_store.acting():
            for left in book_store.contracts(store.ACTIVE):
                if left.slot == LIVE_TEST_SLOT:
                    _finish_live_test(test, left)
            return _run_live_test(test, amount_xch)
    except store.BusyError as exc:
        raise SystemExit(f"{exc}. Stop the other process, then run live-test again.") from exc


def _run_live_test(test: _LiveTest, amount_xch: float) -> int:
    coins, address, custody = test.coins, test.address, test.custody
    amount = int(round(amount_xch * config.XCH_MOJOS))
    fee = test.fee
    xch = test.free_xch()
    have = sum(int(coin.amount) for coin in xch)
    # Three fees, or one spare mojo when there is no fee: the cancel needs an
    # XCH coin to start from, and a fee-less mint could use the address's last.
    need = amount + builder.OPTION_COIN_AMOUNT + max(3 * fee, 1)
    print(f"free at that address: {have} mojos; the test needs {need}")
    if have < need:
        raise SystemExit(
            "send a little XCH to the options address and wait for it to confirm "
            "(coins a transaction still waiting already spends are not counted)"
        )

    print(f"2. minting a contract over {amount_xch:g} XCH, expiring in 24 hours")
    expiry = test.now() + 24 * 3600
    terms = builder.OptionTerms(
        builder.Asset(None, amount), builder.Asset(None, amount), expiry, address
    )
    mint_until = test.valid_until()
    minted = builder.build_mint(
        terms, builder.Funding(address, xch=xch), custody, fee=fee, valid_until_seconds=mint_until
    )
    launcher_id = bytes(minted.launcher_id)
    lock = bytes(minted.underlying.puzzle_hash())
    print(f"  launcher id {launcher_id.hex()}")

    def record_mint(bundle: bytes) -> None:
        test.book_store.add_contract(
            launcher_id=launcher_id,
            slot=LIVE_TEST_SLOT,
            side="call",
            underlying_asset=None,
            underlying_amount=amount,
            strike_asset=None,
            strike_amount=amount,
            expiration=expiry,
            creator_puzzle_hash=address,
            bundle=bundle,
            deadline=test.deadline(),
            valid_until=mint_until,
            fee=fee,
            now=test.now(),
        )

    confirmed = test.send(
        minted.coin_spends,
        safety.Expected(
            wallet={None: -(amount + builder.OPTION_COIN_AMOUNT + fee)},
            locked={None: amount},
            other_max={None: builder.OPTION_COIN_AMOUNT},
            max_fee=fee,
        ),
        lock,
        "mint",
        record_mint,
    )
    if not confirmed:
        raise SystemExit("the mint was overtaken by another spend of its coins; nothing is locked")
    loaded = chain.load_option(test.reader, launcher_id, now_seconds=test.now())
    print(
        f"  read back from chain: {loaded.status.value}, owned by us: "
        f"{loaded.owner_puzzle_hash == address}"
    )

    print("3. making a 15 minute offer for it (signed, not sent anywhere)")
    draft = builder.build_offer(
        loaded.option,
        builder.Asset(None, amount),
        address,
        custody,
        expires_at_seconds=test.now() + 900,
    )
    safety.require(
        safety.analyse(draft.coin_spends, ours=[address], locks=[lock]),
        safety.Expected(other_max={None: builder.OPTION_COIN_AMOUNT}),
    )
    signature = test.wallet.sign(draft.coin_spends)
    offer = draft.encode(signature)
    # On record like any other offer, so a take is booked with what it paid.
    test.book_store.add_offer(
        launcher_id=launcher_id,
        price_asset=None,
        price_amount=amount,
        expires_at=draft.expires_at_seconds,
        bundle=pack_bundle(draft.coin_spends, signature),
        offer_text=offer,
        now=test.now(),
    )
    print(f"  offer string ({len(offer)} characters):\n  {offer}")
    print("  to test a take, paste it into another wallet now; otherwise carry on")

    print("4. withdrawing the offer by moving the option to our own address")
    withdraw_until = test.valid_until()
    moved = builder.build_transfer(
        loaded.option,
        address,
        builder.Funding(address, xch=test.free_xch() if fee else []),
        custody,
        fee=fee,
        valid_until_seconds=withdraw_until,
    )

    def record_withdrawal(bundle: bytes) -> None:
        test.book_store.add_broadcast(
            launcher_id=launcher_id,
            kind=store.WITHDRAW,
            bundle=bundle,
            deadline=test.deadline(),
            valid_until=withdraw_until,
            fee=fee,
            now=test.now(),
        )

    withdrawn = test.send(
        moved.coin_spends,
        safety.Expected(
            wallet={None: -fee}, other_max={None: builder.OPTION_COIN_AMOUNT}, max_fee=fee
        ),
        lock,
        "withdrawal",
        record_withdrawal,
    )
    contract = test.book_store.contract(launcher_id)
    assert contract is not None
    if not withdrawn:
        if contract.status != store.SOLD:
            raise SystemExit("the withdrawal was overtaken by another spend; run live-test again")
        when = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(expiry))
        print("  the offer was taken first: the option is sold and the test ends here.")
        print(f"  its collateral can be clawed back after {when}, unless the buyer exercises")
        print("  it first: run live-test again then.")
        return 0
    status = chain.bundle_status(test.reader, draft.coin_spends)
    print(f"  the offer is now: {status.value} (conflict means it can never be taken)")

    print("5. cancelling the contract to get the collateral back")
    _cancel_stage(test, contract)
    after = sum(int(coin.amount) for coin in coins.xch_coins())
    print(f"done. Spendable now: {after} mojos (started with {have}; fees paid: {have - after})")
    print("Now check the wallet's own balance and transaction list agree with this.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m gui.services.options", description=__doc__)
    parser.add_argument("--config", default="config.yaml")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument(
        "--log-file", help="where to log (default: beside the database, as <name>.log)"
    )
    parser.add_argument("--no-log-file", action="store_true", help="log to the terminal only")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="address, funds and contracts")
    commands.add_parser("step", help="run one step and print what it did")
    run = commands.add_parser("run", help="step in a loop")
    run.add_argument("--interval", type=float, default=60.0)
    commands.add_parser("withdraw-all", help="take every open offer off the market")
    watch = commands.add_parser("monitor", help="read-only: log what the desk lists")
    watch.add_argument("--interval", type=float, default=0.0, help="seconds; 0 scans once")
    test = commands.add_parser("live-test", help="guided dust test against the real wallet")
    test.add_argument("--amount-xch", type=float, default=0.0001)
    test.add_argument("--yes", action="store_true", help="do not pause before each spend")
    test.add_argument("--timeout", type=float, default=900.0)
    summary = commands.add_parser("report", help="read-only: what the book did over a period")
    summary.add_argument(
        "--since", default="24h", help="24h, 7d, 90m, a date like 2026-11-15, or all"
    )
    summary.add_argument("--csv", help="also write the per-contract figures to this file")
    args = parser.parse_args(argv)

    root = logging.getLogger()
    level = root.level
    handlers = [_log_to_console(args.verbose)]
    try:
        cfg = _load(args.config)
        path = Path(args.log_file) if args.log_file else default_log_path(cfg)
        if path is not None and not args.no_log_file and args.command not in _NO_LOG_FILE:
            # Log lines are appended, and the file is renamed as it rotates.
            clash = _clash(path, _book_files(cfg, args.config, None))
            if clash is not None:
                print(
                    f"error: log file {path} {clash}; give --log-file a file of its own,"
                    " or pass --no-log-file",
                    file=sys.stderr,
                )
                return 2
            handlers.append(_log_to_file(path))
            log.info("[options] %s, config %s", args.command, args.config)
        if args.command == "report":
            return _report(cfg, args.since, args.csv, args.config, path)
        if args.command == "status":
            return _status(cfg)
        if args.command == "step":
            return _step(cfg)
        if args.command == "run":
            return _run(cfg, max(5.0, args.interval))
        if args.command == "withdraw-all":
            return _withdraw_all(cfg)
        if args.command == "monitor":
            return _monitor(cfg, args.interval)
        try:
            return _live_test(cfg, args.amount_xch, args.yes, args.timeout)
        except KeyboardInterrupt:
            print("\ninterrupted. What was sent is on record: run live-test again to finish it.")
            return 130
    except config.OptionsConfigError as exc:
        print(f"config error: {exc}", file=sys.stderr)
        return 2
    except (
        builder.OptionBuildError,
        safety.SafetyError,
        chain.ChainError,
        chain.InvalidOptionError,
        RuntimeError,
    ) as exc:
        print(f"error: {exc}", file=sys.stderr)
        log.error("[options] %s", exc, extra=REPORTED)
        return 1
    finally:
        # main() may be called more than once in a process (the tests do).
        for handler in handlers:
            root.removeHandler(handler)
            handler.close()
        root.setLevel(level)


if __name__ == "__main__":
    raise SystemExit(main())
