"""The options book on a background thread, for the GUI's Options switch.

``python -m gui.services.options run`` steps the book in a terminal. This runs
the same loop inside the GUI process instead, so the installed app can switch
it Off, to Rehearse (``dry_run``: builds and checks, signs nothing) or to Live,
with no terminal and no command. Nothing here imports Qt: the GUI starts and
stops a :class:`OptionsRunner` and reads its :class:`Snapshot` on a timer.

What runs is what the command line runs:

* the book from :func:`runtime.build`, over ``config.yaml`` with
  ``secrets.yaml`` laid over it, as the GUI and the engine read them, with a
  relative ``database_path`` read beside config.yaml
  (:func:`config.resolve_database_path`, the command line's rule too);
* ``dry_run`` set by the switch, whatever ``options.dry_run`` says: Rehearse is
  always a rehearsal, and Live is live only because the operator chose it;
* with ``options.wallet.share_trading_wallet``, the engine check before every
  step and every signature (:mod:`engine_guard`). A live step is refused while
  the engine runs, and Live cannot be switched on while it does;
* the book's own log file beside its database (``options_jobs.log``), and the
  step journal in it, as the command line keeps them.

The other direction, the engine waiting for the book, is
:func:`engine_start_window`: EngineBridge holds it while it checks and
launches the engine. It covers every book this process has run, not just the
one config.yaml names now, since Settings can switch config.yaml under a book
that is still running or still has contracts open. A book holds the engine
while it is Live, while it is in the middle of a step, and while it has
contracts open; with ``options.wallet.engine_beside_open_contracts``, only
while its own transactions are still in flight instead of the last.

The book's SQLite store belongs to the thread that opened it, so everything the
GUI shows is copied into the snapshot on that thread, after each step.

[AUTO-FENCE] With ``options.wallet.engine_fence: auto`` the runner keeps the
engine's fence flag (:func:`fence.set_flag`) while the book is in use: raised
before a run starts, and checked again when a run ends and when the page
attaches, against the book's record -- down only once the book is off with no
contract open and no transaction unsettled (:func:`in_use_reason`). A Live
run beside a running engine then waits, step by step, until the engine
reports its fence up; it is not refused at the switch.
"""

from __future__ import annotations

import collections
import contextlib
import dataclasses
import enum
import logging
import os
import threading
import time
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

from gui.services.options import config, engine_guard, fence, runtime, store
from gui.services.options.service import ENGINE_REFUSED

log = logging.getLogger(__name__)

#: Seconds between steps, as ``run --interval 60`` on the command line.
DEFAULT_INTERVAL = 60.0
#: How long a switch from the GUI waits for a step in progress before it says
#: "try again" rather than hold the window. Closing the app waits longer.
SWITCH_WAIT = 2.0
#: [review] Why a run that new settings stopped is not started again by itself.
WALLET_CHANGED = (
    "the saved settings change the wallet, network or record the book uses. If the trading "
    "engine shares the wallet, restart it with them first; then switch the book back on"
)
#: Lines of recent activity kept for the panel.
_HISTORY = 200
#: The book's log file takes what this package logs, not the whole app's log.
_PACKAGE_LOGGER = "gui.services.options"


class Mode(enum.StrEnum):
    OFF = "off"
    REHEARSE = "rehearse"
    LIVE = "live"


def live_mode(mode: Mode) -> bool:
    return mode is Mode.LIVE


class RunnerError(RuntimeError):
    """The switch cannot be moved as asked; the message says why."""


@dataclasses.dataclass(frozen=True)
class ContractView:
    slot: str
    status: str
    launcher_id: str  # hex
    expiration: int
    underlying_amount: int
    #: The collateral in its own asset: "0.5 XCH", or "100 quote" for a put.
    collateral: str
    live_offers: int


@dataclasses.dataclass(frozen=True)
class Snapshot:
    """What the panel shows. Plain values, safe to read from any thread."""

    mode: Mode = Mode.OFF
    running: bool = False
    #: Switched off, with the step in progress still finishing.
    stopping: bool = False
    steps: int = 0
    last_step_at: float | None = None
    #: The engine check's last answer when the wallet is shared, else "".
    engine: str = ""
    #: Why the last step did nothing, if it was refused or failed.
    held: str = ""
    #: Why the last run never stepped at all (the book could not be built),
    #: until the next start. ``held`` says it too, but outlives it.
    start_error: str = ""
    market: str = ""
    contracts: tuple[ContractView, ...] = ()
    fees_paid_mojos: int = 0
    recent: tuple[str, ...] = ()
    #: Lines of activity ever written, so a reader can tell what is new once
    #: ``recent`` is full and stops growing.
    recent_count: int = 0
    #: The options address against its target, as the last step that checked
    #: its top-ups saw it ("1.2 of 1.5 XCH, 2 of 2 quote"); "" before one has.
    sub_account: str = ""

    @property
    def open_contracts(self) -> int:
        return sum(item.status in store.ACTIVE for item in self.contracts)


# -- config ----------------------------------------------------------------------


def _options_as_if_on(config_path: Path) -> config.OptionsConfig | None:
    """[review] config.yaml's options read as if they were switched on, or None.

    For the fence flag alone: a book switched off since a flag went up still
    names the record and the fence mode that decide whether it comes down.
    """
    from gui.services.config_split import load_merged

    try:
        root = load_merged(Path(config_path))
        section = root.get("options") if isinstance(root, dict) else None
        if not isinstance(section, dict):
            return None
        parsed = config.parse({**root, "options": {**section, "enabled": True}})
        if parsed is None:
            return None
        return config.resolve_database_path(parsed, config_path)
    except Exception:  # noqa: BLE001 - no settings to follow: the flag stays as it is
        return None


def load_options(config_path: Path) -> config.OptionsConfig:
    """The ``options:`` section of config.yaml with secrets.yaml over it, or RunnerError."""
    from gui.services.config_split import load_merged

    try:
        root = load_merged(Path(config_path))
    except Exception as exc:  # noqa: BLE001 - missing file, OSError, or a yaml.YAMLError
        raise RunnerError(f"config.yaml could not be read: {exc}") from exc
    try:
        parsed = config.parse(root)
        if parsed is None:
            raise RunnerError("options are not set up: options.enabled is not true in config.yaml")
        return config.resolve_database_path(parsed, config_path)
    except config.OptionsConfigError as exc:
        raise RunnerError(f"options config: {exc}") from exc


# -- the engine waits for the book -------------------------------------------------

#: Every config.yaml this process has run a book on, and whether that run is
#: Live now. Settings can switch config.yaml under a running book, and a book
#: switched off can still have contracts open; the engine check looks at all.
_BOOKS: dict[str, bool] = {}
_BOOKS_LOCK = threading.Lock()


def _book_key(config_path: Path) -> str:
    return os.path.normcase(os.path.abspath(str(config_path)))


def _set_book(config_path: Path, live: bool) -> None:
    with _BOOKS_LOCK:
        _BOOKS[_book_key(config_path)] = live


def _books(config_path: Path) -> list[tuple[Path, bool]]:
    """The book at ``config_path`` and every other this process has run, with Live flags."""
    key = _book_key(config_path)
    with _BOOKS_LOCK:
        known = dict(_BOOKS)
    paths = {key: known.get(key, False), **known}
    return [(Path(path), live) for path, live in paths.items()]


def _shared_book(config_path: Path) -> tuple[str | None, config.OptionsConfig | None]:
    """(why the engine must wait, None) or (None, the book's config if it shares the wallet).

    No config.yaml, no options section, options switched off, or a book on a
    wallet of its own: (None, None), nothing to wait for. An enabled book that
    shares the trading wallet but cannot be read is a reason to wait: its
    contracts cannot be checked.
    """
    from gui.services.config_split import load_merged

    path = Path(config_path)
    if not path.exists():
        return None, None
    try:
        root = load_merged(path)
    except Exception as exc:  # noqa: BLE001 - fail closed: nothing says no book is there
        return f"config.yaml could not be read to check the options book ({exc})", None
    section = root.get("options") if isinstance(root, dict) else None
    if not isinstance(section, dict) or section.get("enabled") is not True:
        return None, None
    wallet = section.get("wallet")
    if not isinstance(wallet, dict) or wallet.get("share_trading_wallet") is not True:
        return None, None
    try:
        parsed = config.parse(root)
        assert parsed is not None
        return None, config.resolve_database_path(parsed, path)
    except Exception as exc:  # noqa: BLE001 - fail closed
        return (
            "the options book shares the trading wallet, but its settings could not be read "
            f"({exc}); the engine stays stopped until they can be and its contracts checked"
        ), None


def _open_reason(still_open: list[store.Contract]) -> str | None:
    if not still_open:
        return None
    locked = sum(item.underlying_amount for item in still_open if item.underlying_asset is None)
    quote = sum(1 for item in still_open if item.underlying_asset is not None)
    held = f"{locked / config.XCH_MOJOS:g} XCH locked" + (
        f", {quote} put(s) holding the quote asset" if quote else ""
    )
    return (
        f"the options book shares the trading wallet and has {len(still_open)} contract(s) "
        f"open ({held}). The engine must stay stopped until they are closed: its offers would "
        "spend the book's coins. Close them first (the options book cancels unsold contracts "
        "before expiry, and claws back after it)."
    )


def _in_flight_reason(book_store: store.OptionStore) -> str | None:
    """Why the engine must wait for the book's own transactions, or None.

    Read for a book with ``options.wallet.engine_beside_open_contracts``: its
    open contracts do not hold the engine, but a transaction it has sent and
    the chain has not ruled on does. The book pushes bundles it built itself,
    so the wallet does not know their coins are being spent and could hand them
    to an engine offer. Every unresolved one (pending or stalled) holds the
    engine until a step records its verdict -- confirmed, overtaken, or lapsed,
    which the book decides only from the chain's own clock read 32 blocks deep
    (``service._settle_broadcasts``), never from this machine's. A dry-run step
    records verdicts too, so Rehearse is enough to settle them.
    """
    waiting = book_store.unresolved()
    if not waiting:
        return None
    kinds = ", ".join(sorted({row.kind for row in waiting}))
    return (
        f"the options book has {len(waiting)} transaction(s) waiting to confirm ({kinds}). "
        "The engine waits so that its offers cannot pick the same coins: leave the book on "
        "(Live or Rehearse) until its record shows them settled, then start the engine."
    )


def in_use_reason(book_store: store.OptionStore) -> str | None:
    """[AUTO-FENCE] Why the book's record keeps the fence up, or None.

    A contract open (its next cancel or clawback needs the book's coins where
    it left them) or a transaction the chain has not ruled on (the wallet does
    not know its coins are being spent). Being switched on is the runner's to
    add.
    """
    still_open = book_store.contracts(store.ACTIVE)
    if still_open:
        return f"{len(still_open)} contract(s) open"
    waiting = book_store.unresolved()
    if waiting:
        return f"{len(waiting)} transaction(s) unsettled"
    return None


def _hold_reason(cfg: config.OptionsConfig, book_store: store.OptionStore) -> str | None:
    """Why this shared-wallet book holds the engine, from its record; None if it does not.

    By default every open contract does. With
    ``options.wallet.engine_beside_open_contracts`` only the book's
    transactions still in flight do; being Live and stepping are checked by
    the callers.
    """
    if cfg.wallet.engine_fence:
        # [OPTIONS-FENCE] The book spends only fenced coins: neither its open
        # contracts nor its transactions in flight are the engine's to touch.
        return None
    if cfg.wallet.engine_beside_open_contracts:
        return _in_flight_reason(book_store)
    return _open_reason(book_store.contracts(store.ACTIVE))


def _read_hold(cfg: config.OptionsConfig) -> str | None:
    if not Path(cfg.database_path).exists():
        return None
    book_store = store.OptionStore(cfg.database_path)
    try:
        return _hold_reason(cfg, book_store)
    finally:
        book_store.close()


def engine_start_blocker(config_path: Path) -> str | None:
    """Why the trading engine must not start now, or None. For display: takes no lock.

    The engine start itself goes through :func:`engine_start_window`, which
    holds each book's acting lock while the engine is launched.
    """
    for path, live in _books(Path(config_path)):
        why, cfg = _shared_book(path)
        if why:
            return why
        if cfg is None:
            continue
        if live and not cfg.wallet.engine_fence:
            return (
                "the options book is Live on the trading wallet; switch it off on the "
                "Options page first"
            )
        try:
            why = _read_hold(cfg)
        except Exception as exc:  # noqa: BLE001 - fail closed
            why = (
                "the options book shares the trading wallet and its record could not be read "
                f"({exc}); the engine stays stopped until it can be"
            )
        if why:
            return why
    return None


@contextlib.contextmanager
def engine_start_window(config_path: Path) -> Iterator[str | None]:
    """Yield why the engine must not start, or None, holding the books still while it launches.

    For each book sharing the trading wallet, its acting lock (the one every
    step takes) is held from the check to the end of the ``with`` block, so no
    step can mint between "nothing is open" and the engine being up. A book in
    the middle of a step is a reason to wait. By the time the lock is let go
    the engine's process exists, and the next step sees it and holds. With
    ``engine_beside_open_contracts`` the lock is taken all the same: open
    contracts no longer hold the engine, but a step in progress still does.
    """
    with contextlib.ExitStack() as held:
        reason: str | None = None
        for path, live in _books(Path(config_path)):
            why, cfg = _shared_book(path)
            if why or cfg is None:
                reason = why
                if reason:
                    break
                continue
            if live and not cfg.wallet.engine_fence:
                reason = (
                    "the options book is Live on the trading wallet; switch it off on the "
                    "Options page first"
                )
                break
            if not Path(cfg.database_path).exists():
                continue
            try:
                book_store = store.OptionStore(cfg.database_path)
                held.callback(book_store.close)
                held.enter_context(book_store.acting())
                reason = _hold_reason(cfg, book_store)
            except store.BusyError:
                reason = (
                    "the options book is in the middle of a step; start the engine again in "
                    "a moment"
                )
            except Exception as exc:  # noqa: BLE001 - fail closed
                reason = (
                    "the options book shares the trading wallet and its record could not be "
                    f"read ({exc}); the engine stays stopped until it can be"
                )
            if reason:
                break
        yield reason


def fence_identity(cfg: config.OptionsConfig) -> str:
    """The book address as the engine publishes its fence: "<puzzle hash>@<network>".

    "" when the address cannot be read, which matches no fence.
    """
    try:
        import chia_wallet_sdk

        puzzle_hash = bytes(chia_wallet_sdk.Address.decode(cfg.wallet.address).puzzle_hash)
        return f"{puzzle_hash.hex()}@{cfg.network}"
    except Exception:  # noqa: BLE001 - unreadable: matches nothing
        return ""


def contract_views(cfg: config.OptionsConfig, book_store: Any) -> tuple[ContractView, ...]:
    """Every contract on record, as the page shows it."""

    def collateral(item: Any) -> str:
        if item.underlying_asset is None:
            return f"{int(item.underlying_amount) / config.XCH_MOJOS:g} XCH"
        return f"{int(item.underlying_amount) / cfg.quote_unit_mojos:g} quote"

    return tuple(
        ContractView(
            slot=item.slot,
            status=item.status,
            launcher_id=item.launcher_id.hex(),
            expiration=int(item.expiration),
            underlying_amount=int(item.underlying_amount),
            collateral=collateral(item),
            live_offers=len(book_store.offers(item.launcher_id, "live")),
        )
        for item in book_store.contracts()
    )


def sub_account_text(held: dict[str, tuple[int, int]], cfg: config.OptionsConfig) -> str:
    """``{"XCH": (held, target), ...}`` as "1.2 of 1.5 XCH, 2 of 2 quote"."""
    parts = []
    for what, (have, want) in held.items():
        unit = config.XCH_MOJOS if what == "XCH" else cfg.quote_unit_mojos
        name = "XCH" if what == "XCH" else "quote"
        parts.append(f"{have / unit:g} of {want / unit:g} {name}")
    return ", ".join(parts)


# -- the runner ------------------------------------------------------------------


@dataclasses.dataclass
class _Run:
    thread: threading.Thread
    stop: threading.Event
    mode: Mode
    #: The settings the run started with, as config.yaml gave them.
    loaded: config.OptionsConfig | None = None


class OptionsRunner:
    """Steps the book on its own thread until stopped. One per GUI page."""

    def __init__(
        self,
        config_path: Path,
        *,
        interval: float = DEFAULT_INTERVAL,
        build: Callable[..., tuple[Any, Any, Any]] = runtime.build,
        engine_state: Callable[[str], engine_guard.EngineState] | None = None,
        clock: Callable[[], float] = time.time,
        fence_state: Callable[[str], engine_guard.FenceState] | None = None,
    ) -> None:
        self._config_path = Path(config_path)
        self._interval = max(0.01, float(interval))
        self._build = build
        self._engine_state = engine_state
        self._fence_state = fence_state
        self._clock = clock
        self._lock = threading.Lock()
        self._run: _Run | None = None
        #: [review] The settings the last run started with, for restart_refusal().
        self._last_loaded: config.OptionsConfig | None = None
        self._snapshot = Snapshot()
        self._recent: collections.deque[str] = collections.deque(maxlen=_HISTORY)
        #: [AUTO-FENCE] Serialises the flag with the run it answers for: a run
        #: is created and the flag raised under it, and a run ending clears
        #: itself and checks the flag under it, so an ending run can never
        #: lower the flag a new one has just raised.
        self._fence_lock = threading.Lock()
        #: [FEE-COIN] The app is closing with the book on: the run ending
        #: leaves the flag up (see stop(keep_fence=True)).
        self._keep_fence = False
        #: [review] The flag the last run raised (beside the engine's database
        #: as config.yaml then had it), judged with that run's book.
        self._last_flag: Path | None = None

    @property
    def config_path(self) -> Path:
        return self._config_path

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
        up, Live asked for while the engine shares the wallet and may be up, or
        the previous run still finishing a step.
        """
        mode = Mode(mode)
        if mode is Mode.OFF:
            self.stop()
            return
        cfg = load_options(self._config_path)
        # [AUTO-FENCE] Not refused here: switching on is what raises the flag
        # the engine's fence follows, and each live step waits for it.
        follows = cfg.wallet.fence_follows_book
        if mode is Mode.LIVE and cfg.wallet.share_trading_wallet and not follows:
            state = self._engine(cfg)
            if not state.clear and not self._fenced_beside(cfg, state):
                raise RunnerError(
                    "Live refused: the options book shares the trading wallet and the trading "
                    f"engine is {state.describe()}. Stop the engine first"
                    + (
                        f" (its fence: {self._fence(cfg).detail})."
                        if cfg.wallet.engine_fence
                        else "."
                    )
                )
        current = self._run
        if current is not None and current.thread.is_alive():
            if current.mode is mode and not current.stop.is_set():
                return
            # Two runs must never overlap, and the GUI thread must not hang on
            # a slow step: a short wait, then a refusal to try again.
            if not current.stop.is_set():
                self.stop(timeout=SWITCH_WAIT)
            if self._run is not None:
                raise RunnerError(
                    "the previous run is still finishing its step; try again in a moment"
                )
        loaded = self._last_loaded = cfg
        self._keep_fence = False
        cfg = dataclasses.replace(cfg, dry_run=mode is not Mode.LIVE)
        stop = threading.Event()
        thread = threading.Thread(
            target=self._loop, args=(cfg, mode, stop), name=f"options-{mode.value}", daemon=True
        )
        with self._fence_lock:
            if cfg.wallet.fence_follows_book:
                flag = self._last_flag = self._fence_flag()
                if not fence.raise_flag(flag, f"switched on ({mode.value})") and live_mode(mode):
                    # [review] Nothing else would keep the app's own spends off
                    # the options coins while the book signs.
                    raise RunnerError(
                        f"Live refused: the engine's fence flag could not be written at {flag}; "
                        "the book does not sign without it"
                    )
            self._run = _Run(thread=thread, stop=stop, mode=mode, loaded=loaded)
        _set_book(self._config_path, live=mode is Mode.LIVE)
        with self._lock:
            self._snapshot = dataclasses.replace(
                self._snapshot,
                mode=mode,
                running=True,
                stopping=False,
                held="",
                start_error="",
                steps=0,
            )
        self._note(f"switched to {mode.value}")
        thread.start()

    def stop(self, timeout: float = 30.0, *, keep_fence: bool = False) -> None:
        """Stop after the step in progress, if any. Safe at any time.

        Offers already out expire on their own (15 minutes by default), and
        collateral waits for the next step after expiry to be clawed back. A
        step that outlives ``timeout`` is left to finish: the run stays tracked
        (``stopping``) until it does, and nothing new starts beside it.

        [FEE-COIN] ``keep_fence``: the app is closing with the book on, and the
        next start resumes it. The engine's fence flag stays up, so the engine
        a restart brings up is fenced from its first moment, not only once the
        next app raises the flag again.
        """
        current = self._run
        if current is None:
            return
        self._keep_fence = keep_fence
        _set_book(self._config_path, live=False)  # still watched: contracts may stay open
        current.stop.set()
        if current.thread is not threading.current_thread():
            current.thread.join(timeout)
        finished = not current.thread.is_alive()
        if finished and self._run is current:
            self._run = None
        with self._lock:
            was = self._snapshot.mode
            self._snapshot = dataclasses.replace(
                self._snapshot, mode=Mode.OFF, running=not finished, stopping=not finished
            )
        if was is not Mode.OFF:
            self._note("switched off" if finished else "switching off after the step in progress")

    @property
    def finishing(self) -> bool:
        """Whether a run told to stop is still in its step: nothing new can start yet."""
        current = self._run
        return current is not None and current.stop.is_set()

    def restart_refusal(self) -> str:
        """[review] Why a run new settings stopped must not start again by itself, or "".

        Saved settings that change the wallet the book signs from (its daemon,
        key, address or fence), its network or its record are the operator's
        to take up. On a shared wallet a step logs the daemon in to the book's
        key, and the trading engine takes up a new key only when it restarts.
        """
        try:
            fresh = load_options(self._config_path)
        except RunnerError as exc:
            return str(exc)
        last = self._last_loaded
        if last is not None and (fresh.wallet, fresh.network, fresh.database_path) != (
            last.wallet,
            last.network,
            last.database_path,
        ):
            return WALLET_CHANGED
        return ""

    def stop_for_new_settings(self) -> Mode | None:
        """[review] Settings saved config.yaml under a run: stop it if its settings changed.

        A run reads its settings when it starts, so a change reaches it only by
        a restart. A run whose settings changed (or that config.yaml no longer
        sets up) is stopped after its step, with the short wait the switch
        uses, and its mode is returned for the caller to start again once
        :attr:`finishing` is over. None, and nothing stopped, when nothing runs
        or nothing the book reads changed.
        """
        current = self._run
        if current is None or current.stop.is_set():
            return None
        try:
            fresh: config.OptionsConfig | None = load_options(self._config_path)
        except RunnerError:
            fresh = None  # switched off or broken: the run must not go on regardless
        if fresh is not None and fresh == current.loaded:
            return None
        self._note("new settings saved: switching off to take them up")
        # [FEE-COIN] Back on in a moment: the fence stays up in between. A
        # restart refused after all leaves it to the page's next check.
        self.stop(timeout=SWITCH_WAIT, keep_fence=True)
        return current.mode

    # -- the loop (its own thread) -------------------------------------------

    def _open_log(self, cfg: config.OptionsConfig, mode: Mode) -> logging.Handler | None:
        """The book's own log file, as the command line writes it, for the length of a run.

        It takes what the options modules log, beside the app's own log, which
        keeps everything. A file that cannot be opened is reported and the run
        goes on without it.
        """
        path = runtime.log_path(cfg)
        if path is None:
            return None
        try:
            handler = runtime.log_handler(path)
        except OSError as exc:
            log.warning("options: the book's log file %s could not be opened: %s", path, exc)
            return None
        logging.getLogger(_PACKAGE_LOGGER).addHandler(handler)
        log.info("[options] %s run started from the app (config %s)", mode.value, self._config_path)
        return handler

    # -- [AUTO-FENCE] the engine's fence flag --------------------------------------

    def _fence_flag(self) -> Path:
        """Where the engine of this config.yaml looks for the flag (beside its database)."""
        from gui.services.config_split import load_merged

        try:
            root = load_merged(self._config_path)
        except Exception:  # noqa: BLE001 - the default database's folder, as the bridge's
            root = None
        return fence.flag_path(self._config_path.parent, root)

    def sync_fence(self) -> None:
        """Raise or lower the flag to match the book; for the page, when it attaches.

        Up while a run is on or the record has a contract open or a transaction
        unsettled; down otherwise. Only with ``engine_fence: auto``. Never
        raises: options that cannot be read at all leave the flag as it is.

        [review] With options switched off, or the fence no longer following
        the book, a flag this app left up (for a restart, say) still follows
        the book it was raised for: the settings the last run had, or
        config.yaml's read as if the book were on.
        """
        books = self._fence_books()
        if not books:
            return
        with self._fence_lock:
            self._keep_fence = False
            self._sync_fence_locked(books)

    def settle_fence(self) -> None:
        """[review] A restart that is not to happen: the flag follows the book again.

        ``stop(keep_fence=True)`` left the flag up for a run that does not
        come (its settings refused, or switched off meanwhile).
        """
        self.sync_fence()

    def _fence_books(self, *also: config.OptionsConfig) -> list[config.OptionsConfig]:
        """[review] The books the flag answers to, one per record.

        config.yaml's as it is now (or read as if switched on), the book the
        last run had, and ``also``: a save can switch the record or the fence
        mode under a flag raised for the old book, which must still be judged
        by that book's record.
        """
        try:
            current: config.OptionsConfig | None = load_options(self._config_path)
        except RunnerError:
            current = _options_as_if_on(self._config_path)
        books: list[config.OptionsConfig] = []
        for cfg in (*also, current, self._last_loaded):
            if cfg is None or not cfg.wallet.fence_follows_book:
                continue
            if all(Path(cfg.database_path) != Path(b.database_path) for b in books):
                books.append(cfg)
        return books

    def _sync_fence_locked(self, books: list[config.OptionsConfig]) -> None:
        """Caller holds ``_fence_lock``. An unreadable record keeps the flag up.

        [review] The flag is lowered only when nothing could still be using
        any of ``books``: no run here, nothing open or unsettled in any of
        their records, no other process mid-step (each record is checked
        again holding its acting lock), and no other live process that raised
        it last -- a run from a terminal, between its steps. The flag beside
        the engine's database as config.yaml has it now, and the one the last
        run raised (a save can move it), are judged alike.
        """
        flags = [self._fence_flag()]
        if self._last_flag is not None and self._last_flag not in flags:
            flags.append(self._last_flag)
        if self._run is not None:
            for flag in flags:
                fence.raise_flag(flag, f"switched on ({self._run.mode.value})")
            return
        records: list[store.OptionStore] = []
        try:
            for cfg in books:
                if Path(cfg.database_path).exists():
                    records.append(store.OptionStore(cfg.database_path))
            why = next((r for r in map(in_use_reason, records) if r is not None), None)
            if why is not None:
                for flag in flags:
                    if not fence.flag_up(flag):
                        fence.raise_flag(flag, why)
                return
            for flag in flags:
                if self._raised_elsewhere(flag):
                    continue
                with contextlib.ExitStack() as held:
                    for record in records:
                        held.enter_context(record.acting())
                    if all(in_use_reason(record) is None for record in records):
                        fence.lower_flag(flag)
        except store.BusyError:
            return  # another process is mid-step: the flag stays
        except Exception as exc:  # noqa: BLE001 - fail up
            for flag in flags:
                fence.raise_flag(flag, f"its record could not be read ({exc})")
        finally:
            for record in records:
                record.close()

    @staticmethod
    def _raised_elsewhere(flag: Path) -> bool:
        """Whether the flag was last raised by another process that is still alive."""
        owner = fence.flag_owner(flag)
        return owner is not None and owner != os.getpid() and not fence.process_gone(owner)

    def _engine(self, cfg: config.OptionsConfig) -> engine_guard.EngineState:
        check = self._engine_state or engine_guard.engine_state
        return check(cfg.wallet.engine_metrics_url)

    def _fence(self, cfg: config.OptionsConfig) -> engine_guard.FenceState:
        check = self._fence_state or engine_guard.fence_state
        return check(cfg.wallet.engine_metrics_url)

    def _fenced_beside(self, cfg: config.OptionsConfig, state: engine_guard.EngineState) -> bool:
        """[OPTIONS-FENCE] A running engine the book may act beside: its fence holds."""
        if not (cfg.wallet.engine_fence and state.running):
            return False
        return self._fence(cfg).holds_for(fence_identity(cfg))

    def _note(self, text: str) -> None:
        stamp = time.strftime("%H:%M:%S", time.localtime(self._clock()))
        with self._lock:
            self._recent.append(f"{stamp}  {text}")
            self._snapshot = dataclasses.replace(
                self._snapshot,
                recent=tuple(self._recent),
                recent_count=self._snapshot.recent_count + 1,
            )

    def _loop(self, cfg: config.OptionsConfig, mode: Mode, stop: threading.Event) -> None:
        try:
            try:
                book, book_store, _coins = self._build(cfg)
            except Exception as exc:  # noqa: BLE001 - reported, and the switch goes back to off
                log.exception("options: the book could not be started")
                self._note(f"could not start: {exc}")
                _set_book(self._config_path, live=False)
                with self._lock:
                    self._snapshot = dataclasses.replace(
                        self._snapshot,
                        mode=Mode.OFF,
                        held=f"could not start: {exc}",
                        start_error=f"could not start: {exc}",
                    )
                return
            book.step_interval = self._interval  # kept in the journal for the report
            book_log = self._open_log(cfg, mode)
            try:
                while not stop.is_set():
                    self._one_step(cfg, mode, book, book_store)
                    stop.wait(self._interval)
            finally:
                if book_log is not None:
                    log.info("[options] %s run stopped", mode.value)
                    logging.getLogger(_PACKAGE_LOGGER).removeHandler(book_log)
                    book_log.close()
                try:
                    book_store.close()
                except Exception as exc:  # noqa: BLE001 - closing is best effort
                    log.debug("options: closing the book's store failed: %s", exc)
        finally:
            # This run is over: whoever was waiting on it can start the next.
            with self._lock:
                self._snapshot = dataclasses.replace(self._snapshot, running=False, stopping=False)
            with self._fence_lock:
                current = self._run
                if current is not None and current.stop is stop:
                    self._run = None
                if self._run is None and cfg.wallet.fence_follows_book and not self._keep_fence:
                    self._sync_fence_locked(self._fence_books(cfg))

    def _one_step(self, cfg: config.OptionsConfig, mode: Mode, book: Any, book_store: Any) -> None:
        engine, held = "", ""
        if cfg.wallet.fence_follows_book:
            # [review] Refreshed every step, so the flag names this process
            # while it acts; a Live step does not sign without it.
            flag = self._fence_flag()
            if not fence.raise_flag(flag, f"switched on ({mode.value})") and live_mode(mode):
                held = (
                    f"the engine's fence flag could not be written at {flag}; nothing is "
                    "signed until it can be"
                )
                book.journal(ENGINE_REFUSED, held, now=int(self._clock()))
                self._record(cfg, book_store, None, engine, held)
                return
        if cfg.wallet.share_trading_wallet:
            state = self._engine(cfg)
            engine = state.describe()
            if mode is Mode.LIVE and not state.clear and not self._fenced_beside(cfg, state):
                held = (
                    "refused, trading engine up: a live step would sign from the wallet the "
                    f"engine trades through, and the engine is {engine}"
                )
                if cfg.wallet.fence_follows_book:
                    held = (
                        "waiting for the trading engine to put its fence up: it follows the "
                        "book within seconds (engine v0.10.53 or later; an older engine never "
                        f"does -- stop or update it). The engine is {engine}; its fence: "
                        f"{self._fence(cfg).detail}"
                    )
                # As the command line does: the step journal counts the time held.
                book.journal(ENGINE_REFUSED, engine, now=int(self._clock()))
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
            contracts = contract_views(cfg, book_store)
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
        sub_account = self._snapshot.sub_account
        if report is not None and report.sub_account:
            sub_account = sub_account_text(report.sub_account, cfg)
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
                sub_account=sub_account,
            )

    def contracts_on_record(self) -> tuple[ContractView, ...]:
        """The book's contracts as its record has them: for the page while the book is off.

        A book switched off since launch has not stepped, so its snapshot has
        no contracts; its record does. () when there is none.

        [review] Read from a copy (:meth:`store.OptionStore.snapshot`): looking
        never upgrades an older book's file in place, and a record on a
        read-only disk still shows.
        """
        try:
            cfg = load_options(self._config_path)
        except RunnerError:
            return ()
        if not Path(cfg.database_path).exists():
            return ()
        book_store = store.OptionStore.snapshot(cfg.database_path)
        try:
            return contract_views(cfg, book_store)
        finally:
            book_store.close()
