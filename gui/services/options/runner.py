"""The options book on a background thread, for the GUI's Options switch.

``python -m gui.services.options run`` steps the book in a terminal. This runs
the same loop inside the GUI process instead, so the installed app can switch
it Off, to Rehearse (``dry_run``: builds and checks, signs nothing) or to Live,
with no terminal and no command. Nothing here imports Qt: the GUI starts and
stops a :class:`OptionsRunner` and reads its :class:`Snapshot` on a timer.

What runs is what the command line runs:

* the book from :func:`runtime.build`, over ``config.yaml`` with
  ``secrets.yaml`` laid over it, as the GUI and the engine read them;
* ``dry_run`` set by the switch, whatever ``options.dry_run`` says: Rehearse is
  always a rehearsal, and Live is live only because the operator chose it;
* with ``options.wallet.share_trading_wallet``, the engine check before every
  step and every signature (:mod:`engine_guard`). A live step is refused while
  the engine runs, and Live cannot be switched on while it does.

The book's SQLite store belongs to the thread that opened it, so everything the
GUI shows is copied into the snapshot on that thread, after each step.
"""

from __future__ import annotations

import collections
import dataclasses
import enum
import logging
import threading
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from gui.services.options import config, engine_guard, runtime, store

log = logging.getLogger(__name__)

#: Seconds between steps, as ``run --interval 60`` on the command line.
DEFAULT_INTERVAL = 60.0
#: Lines of recent activity kept for the panel.
_HISTORY = 200


class Mode(enum.StrEnum):
    OFF = "off"
    REHEARSE = "rehearse"
    LIVE = "live"


class RunnerError(RuntimeError):
    """The switch cannot be moved as asked; the message says why."""


@dataclasses.dataclass(frozen=True)
class ContractView:
    slot: str
    status: str
    launcher_id: str  # hex
    expiration: int
    underlying_amount: int
    live_offers: int


@dataclasses.dataclass(frozen=True)
class Snapshot:
    """What the panel shows. Plain values, safe to read from any thread."""

    mode: Mode = Mode.OFF
    running: bool = False
    steps: int = 0
    last_step_at: float | None = None
    #: The engine check's last answer when the wallet is shared, else "".
    engine: str = ""
    #: Why the last step did nothing, if it was refused or failed.
    held: str = ""
    market: str = ""
    contracts: tuple[ContractView, ...] = ()
    fees_paid_mojos: int = 0
    recent: tuple[str, ...] = ()
    #: Lines of activity ever written, so a reader can tell what is new once
    #: ``recent`` is full and stops growing.
    recent_count: int = 0

    @property
    def open_contracts(self) -> int:
        return sum(item.status in store.ACTIVE for item in self.contracts)


def resolve_database_path(cfg: config.OptionsConfig, config_path: Path) -> config.OptionsConfig:
    """``options.database_path`` made absolute against the config file's folder.

    The command line reads a relative path against the folder it is run from.
    The installed GUI's working folder is the program's own, where nothing may
    be written, so the GUI reads it against the folder config.yaml is in.
    """
    path = Path(cfg.database_path)
    if path.is_absolute() or cfg.database_path == ":memory:":
        return cfg
    return dataclasses.replace(cfg, database_path=str(Path(config_path).parent / path))


def load_options(config_path: Path) -> config.OptionsConfig:
    """The ``options:`` section of config.yaml with secrets.yaml over it, or RunnerError."""
    from gui.services.config_split import load_merged

    try:
        root = load_merged(Path(config_path))
    except Exception as exc:  # noqa: BLE001 - missing file, OSError, or a yaml.YAMLError
        raise RunnerError(f"config.yaml could not be read: {exc}") from exc
    try:
        parsed = config.parse(root)
    except config.OptionsConfigError as exc:
        raise RunnerError(f"options config: {exc}") from exc
    if parsed is None:
        raise RunnerError("options are not set up: options.enabled is not true in config.yaml")
    return resolve_database_path(parsed, Path(config_path))


def open_contracts(cfg: config.OptionsConfig) -> list[store.Contract]:
    """Contracts with collateral still locked, read from the book's own record.

    Read-only, and safe beside a running book: SQLite allows it.
    """
    if not Path(cfg.database_path).exists():
        return []
    book_store = store.OptionStore(cfg.database_path)
    try:
        return book_store.contracts(store.ACTIVE)
    finally:
        book_store.close()


def engine_start_blocker(config_path: Path) -> str | None:
    """Why the trading engine must not start now, or None.

    With ``options.wallet.share_trading_wallet`` the engine has to stay stopped
    while the options book has contracts open: its offers would spend the
    book's coins and its balances would count the book's collateral. Asked by
    the GUI before every engine start. Options not set up, or a dedicated
    wallet, never block.
    """
    try:
        cfg = load_options(Path(config_path))
    except RunnerError:
        return None
    if not cfg.wallet.share_trading_wallet:
        return None
    try:
        still_open = open_contracts(cfg)
    except Exception as exc:  # noqa: BLE001 - a record that cannot be read blocks: fail closed
        return (
            "the options book shares the trading wallet and its record could not be read "
            f"({exc}); the engine stays stopped until it can be"
        )
    if not still_open:
        return None
    locked = sum(item.underlying_amount for item in still_open if item.underlying_asset is None)
    return (
        f"the options book shares the trading wallet and has {len(still_open)} contract(s) open "
        f"({locked / config.XCH_MOJOS:g} XCH locked). The engine must stay stopped until they "
        "are closed: its offers would spend the book's coins. Close them first (the options "
        "book cancels unsold contracts before expiry, and claws back after it)."
    )


class OptionsRunner:
    """Steps the book on its own thread until stopped. One per GUI."""

    def __init__(
        self,
        config_path: Path,
        *,
        interval: float = DEFAULT_INTERVAL,
        build: Callable[..., tuple[Any, Any, Any]] = runtime.build,
        engine_state: Callable[[str], engine_guard.EngineState] | None = None,
        clock: Callable[[], float] = time.time,
    ) -> None:
        self._config_path = Path(config_path)
        self._interval = max(0.01, float(interval))
        self._build = build
        self._engine_state = engine_state
        self._clock = clock
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._thread: threading.Thread | None = None
        self._snapshot = Snapshot()
        self._recent: collections.deque[str] = collections.deque(maxlen=_HISTORY)

    # -- control (any thread) ------------------------------------------------

    @property
    def mode(self) -> Mode:
        with self._lock:
            return self._snapshot.mode

    def snapshot(self) -> Snapshot:
        with self._lock:
            return self._snapshot

    def start(self, mode: Mode) -> None:
        """Run the book in ``mode`` (Rehearse or Live), stopping a run in another mode first.

        Raises RunnerError, and changes nothing, when it cannot: options not set
        up, or Live asked for while the engine shares the wallet and may be up.
        """
        mode = Mode(mode)
        if mode is Mode.OFF:
            self.stop()
            return
        cfg = load_options(self._config_path)
        if mode is Mode.LIVE and cfg.wallet.share_trading_wallet:
            state = self._engine(cfg)
            if not state.clear:
                raise RunnerError(
                    "Live refused: the options book shares the trading wallet and the trading "
                    f"engine is {state.describe()}. Stop the engine first."
                )
        if self.mode is mode and self._alive():
            return
        self.stop()
        cfg = dataclasses.replace(cfg, dry_run=mode is not Mode.LIVE)
        self._wake.clear()
        with self._lock:
            self._snapshot = dataclasses.replace(
                self._snapshot, mode=mode, running=True, held="", steps=0
            )
        self._note(f"switched to {mode.value}")
        self._thread = threading.Thread(
            target=self._loop, args=(cfg, mode), name=f"options-{mode.value}", daemon=True
        )
        self._thread.start()

    def stop(self, timeout: float = 30.0) -> None:
        """Stop after the step in progress, if any. Safe at any time.

        Offers already out expire on their own (15 minutes by default), and
        collateral waits for the next step after expiry to be clawed back.
        """
        thread = self._thread
        self._wake.set()
        if thread is not None and thread.is_alive() and thread is not threading.current_thread():
            thread.join(timeout)
        self._thread = None
        with self._lock:
            was = self._snapshot.mode
            self._snapshot = dataclasses.replace(self._snapshot, mode=Mode.OFF, running=False)
        if was is not Mode.OFF:
            self._note("switched off")

    def _alive(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    # -- the loop (its own thread) -------------------------------------------

    def _engine(self, cfg: config.OptionsConfig) -> engine_guard.EngineState:
        check = self._engine_state or engine_guard.engine_state
        return check(cfg.wallet.engine_metrics_url)

    def _note(self, text: str) -> None:
        stamp = time.strftime("%H:%M:%S", time.localtime(self._clock()))
        with self._lock:
            self._recent.append(f"{stamp}  {text}")
            self._snapshot = dataclasses.replace(
                self._snapshot,
                recent=tuple(self._recent),
                recent_count=self._snapshot.recent_count + 1,
            )

    def _loop(self, cfg: config.OptionsConfig, mode: Mode) -> None:
        try:
            book, book_store, _coins = self._build(cfg)
        except Exception as exc:  # noqa: BLE001 - reported, and the switch goes back to off
            log.exception("options: the book could not be started")
            self._note(f"could not start: {exc}")
            with self._lock:
                self._snapshot = dataclasses.replace(
                    self._snapshot, mode=Mode.OFF, running=False, held=f"could not start: {exc}"
                )
            return
        try:
            while not self._wake.is_set():
                self._one_step(cfg, mode, book, book_store)
                self._wake.wait(self._interval)
        finally:
            try:
                book_store.close()
            except Exception as exc:  # noqa: BLE001 - closing is best effort
                log.debug("options: closing the book's store failed: %s", exc)

    def _one_step(self, cfg: config.OptionsConfig, mode: Mode, book: Any, book_store: Any) -> None:
        engine, held = "", ""
        if cfg.wallet.share_trading_wallet:
            state = self._engine(cfg)
            engine = state.describe()
            if mode is Mode.LIVE and not state.clear:
                held = (
                    "refused, trading engine up: a live step would sign from the wallet the "
                    f"engine trades through, and the engine is {engine}"
                )
        report = None
        if not held:
            try:
                report = book.step()
            except Exception as exc:  # noqa: BLE001 - keep the loop alive, as `run` does
                log.exception("options: step failed")
                held = f"step failed: {exc}"
        self._record(cfg, book_store, report, engine, held)

    def _record(
        self, cfg: config.OptionsConfig, book_store: Any, report: Any, engine: str, held: str
    ) -> None:
        if held:
            self._note(held)
        if report is not None:
            for action in report.actions:
                self._note(action)
            for problem in report.problems:
                self._note(f"note: {problem}")
        try:
            contracts = tuple(
                ContractView(
                    slot=item.slot,
                    status=item.status,
                    launcher_id=item.launcher_id.hex(),
                    expiration=int(item.expiration),
                    underlying_amount=int(item.underlying_amount),
                    live_offers=len(book_store.offers(item.launcher_id, "live")),
                )
                for item in book_store.contracts()
            )
            fees = sum(int(row.fee or 0) for row in book_store.broadcasts("confirmed"))
        except Exception as exc:  # noqa: BLE001 - the panel keeps what it had
            log.warning("options: could not read the book's record: %s", exc)
            contracts, fees = self.snapshot().contracts, self.snapshot().fees_paid_mojos
        market = ""
        if report is not None and report.market is not None:
            view = report.market
            book_text = (
                "no book"
                if view.bid is None or view.ask is None
                else f"{view.bid:.4f}/{view.ask:.4f}"
            )
            market = f"spot {view.spot:.4f}  sigma {view.sigma:.2f}  dexie {book_text}"
        with self._lock:
            self._snapshot = dataclasses.replace(
                self._snapshot,
                steps=self._snapshot.steps + 1,
                last_step_at=self._clock(),
                engine=engine,
                held=held,
                market=market or self._snapshot.market,
                contracts=contracts,
                fees_paid_mojos=fees,
            )
