"""Circuit page: a read-only view of Circuit DAO, the protocol behind BYC (#225).

Circuit lends BYC, a stablecoin, against XCH in collateral vaults. This page
shows what a market maker trading BYC wants to see: the XCH price the
protocol uses, its fees, how much is borrowed, the vaults closest to
liquidation and the liquidation auctions running, the treasury and its
auctions, governance and the oracle's price announcers.

It asks Circuit's public API (https://api.circuitdao.com), and only that. It
holds no key, sends no address and signs nothing: the client refuses any
request body that carries wallet details. It is built on Circuit v1, which is
still in use; v2 will need changes, and an answer of a new shape leaves its
section empty with a note rather than breaking the page.

On by default, for now; Settings > Advanced > Subsystems switches it off.
Switched off, the sidebar entry is hidden and nothing is asked. Switched on,
it reads only while the page is open: when it opens (unless a whole round
started under a minute ago), then every three minutes, eight requests a round
(:mod:`gui.services.circuit.poll`), on a worker thread. Closed, it starts no
new round. Building the page asks nothing, so the switch works both ways
without a restart.
"""

from __future__ import annotations

import logging
import threading
import time
from collections.abc import Callable, Sequence
from typing import Any, Final

from PySide6.QtCore import QObject, Qt, QThread, QTimer, Signal, Slot
from PySide6.QtWidgets import (
    QAbstractItemView,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from gui.services.circuit import model, poll
from gui.theme import COLORS as _C

TEXT_PRIMARY: Final[str] = _C.TEXT_PRIMARY
TEXT_SECONDARY: Final[str] = _C.TEXT_SECONDARY
WARNING_YELLOW: Final[str] = _C.WARNING_YELLOW
LOSS_RED: Final[str] = _C.LOSS_RED

#: Between two rounds of reads while the page is open.
ROUND_EVERY_MS: Final[int] = 180_000
#: Opening the page reads again unless the last round started this recently
#: and was not cut short.
FRESH_FOR_S: Final[float] = 60.0
#: Relative times on the page ("in 21 min") are redrawn this often.
REDRAW_EVERY_MS: Final[int] = 15_000
#: How long closing the app waits for a round to stop.
STOP_WAIT_MS: Final[int] = 3_000

_log = logging.getLogger(__name__)

#: The page's threads that outlived the close wait, with their workers. Held
#: only so that Qt never destroys a QThread still running, which aborts the
#: process; nothing reads this. As permuto's are (gui/services/permuto/live.py).
_ABANDONED: list[tuple[QThread, QObject | None]] = []


class _RoundWorker(QObject):
    """One round of the page's reads, on the page's worker thread."""

    result = Signal(object)  # poll.Result
    done = Signal(int, str)  # reads answered, and why the round ended early ("" if it did not)

    def __init__(self, api: Any, stop: threading.Event, wall: Callable[[], float]) -> None:
        super().__init__()
        self._api = api
        self._stop = stop
        self._wall = wall

    @Slot()
    def run(self) -> None:
        answered, why = 0, ""
        try:
            answered = poll.fetch(
                self._api,
                on_result=self.result.emit,
                should_stop=self._stop.is_set,
                wall=self._wall,
            )
        except poll.Stopped:
            why = "stopped"
        except Exception as exc:  # noqa: BLE001 -- the page reports it; the app goes on
            _log.exception("[Circuit] a round of reads failed")
            why = f"{type(exc).__name__}: {exc}"
        self.done.emit(answered, why)


def _table(columns: Sequence[str]) -> QTableWidget:
    table = QTableWidget(0, len(columns))
    table.setHorizontalHeaderLabels(list(columns))
    table.verticalHeader().setVisible(False)
    table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
    table.setSelectionMode(QAbstractItemView.SelectionMode.NoSelection)
    table.setFocusPolicy(Qt.FocusPolicy.NoFocus)
    table.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    header = table.horizontalHeader()
    header.setSectionResizeMode(QHeaderView.ResizeMode.ResizeToContents)
    header.setStretchLastSection(True)
    # Columns wider than the table bring a scroll bar in under the rows, at
    # any resize: the table grows by its height, and gives it back after.
    table.horizontalScrollBar().rangeChanged.connect(lambda _low, _high: _fit_height(table))
    return table


def _fit_height(table: QTableWidget) -> None:
    """Make ``table`` tall enough for every row, and the horizontal scroll bar if it shows."""
    height = table.horizontalHeader().height() + 2 * table.frameWidth()
    height += sum(table.rowHeight(r) for r in range(table.rowCount()))
    bar = table.horizontalScrollBar()
    if bar.maximum() > bar.minimum():
        height += bar.sizeHint().height()
    table.setFixedHeight(height + 2)


def _fill(table: QTableWidget, rows: Sequence[Sequence[str]], empty: str) -> None:
    """Put ``rows`` in ``table`` (or one row saying ``empty``) and fit its height to them."""
    table.clearSpans()
    if rows:
        table.setRowCount(len(rows))
        for r, row in enumerate(rows):
            for c, text in enumerate(row):
                item = QTableWidgetItem(text)
                item.setToolTip(text)  # a cell too narrow for its text still shows all of it
                table.setItem(r, c, item)
    else:
        table.setRowCount(1)
        table.setItem(0, 0, QTableWidgetItem(empty))
        for c in range(1, table.columnCount()):
            table.setItem(0, c, QTableWidgetItem(""))
        if table.columnCount() > 1:
            table.setSpan(0, 0, 1, table.columnCount())
    _fit_height(table)


def _cells(table: QTableWidget) -> list[list[str]]:
    """The table's text, row by row (for tests and for copying)."""
    out = []
    for r in range(table.rowCount()):
        out.append(
            [
                table.item(r, c).text() if table.item(r, c) is not None else ""
                for c in range(table.columnCount())
            ]
        )
    return out


class CircuitWidget(QWidget):
    """The Circuit page. Asks nothing until it is switched on and opened."""

    def __init__(
        self,
        parent: QWidget | None = None,
        *,
        client_factory: Callable[[threading.Event], Any] | None = None,
        wall: Callable[[], float] = time.time,
    ) -> None:
        super().__init__(parent)
        #: Called once, at the first round: building the page asks nothing.
        self._client_factory = client_factory or poll.page_client
        self._wall = wall
        self._enabled = False
        self._api: Any = None
        self._stop = threading.Event()
        self._thread: QThread | None = None
        self._worker: _RoundWorker | None = None
        #: A round is due as soon as the one still stopping has ended.
        self._round_due = False
        self._answers: dict[str, Any] = {}
        self._answered_at: dict[str, float] = {}
        self._errors: dict[str, str] = {}
        self._round_started_at: float | None = None
        self._round_ended_at: float | None = None
        self._round_seen = 0
        self._round_answered = 0
        self._round_why = ""

        title = QLabel("Circuit DAO")
        title.setStyleSheet(f"font-size: 18px; font-weight: 600; color: {TEXT_PRIMARY};")
        self._refresh_button = QPushButton("Read now")
        self._refresh_button.setToolTip(
            "Ask Circuit's API again now (8 requests, about half a minute)"
        )
        self._refresh_button.clicked.connect(lambda _checked=False: self.refresh())
        top = QHBoxLayout()
        top.addWidget(title)
        top.addStretch(1)
        top.addWidget(self._refresh_button)

        subtitle = QLabel(
            "A read-only view of Circuit, the protocol on Chia that lends the BYC stablecoin "
            "against XCH. It asks Circuit's public API (api.circuitdao.com) while this page is "
            "open. It holds no key and signs nothing. Built for Circuit v1."
        )
        subtitle.setWordWrap(True)
        subtitle.setStyleSheet(f"color: {TEXT_SECONDARY};")
        self._status = QLabel("")
        self._status.setWordWrap(True)
        self._banner = QLabel("")
        self._banner.setWordWrap(True)
        self._banner.setStyleSheet(f"color: {LOSS_RED}; font-weight: 600;")
        self._banner.hide()

        layout = QVBoxLayout(self)
        layout.addLayout(top)
        layout.addWidget(subtitle)
        layout.addWidget(self._status)
        layout.addWidget(self._banner)

        #: Read name -> the notes of the sections built from it.
        self._notes: dict[str, list[QLabel]] = {}
        self._values: dict[str, QLabel] = {}

        protocol, form = self._section("Protocol", ("oracle", "statutes"))
        for key, label in (
            ("price", "XCH price used"),
            ("oracle", "Oracle"),
            ("fee", "Stability fee"),
            ("savings", "Savings rate"),
            ("liquidation", "Liquidation"),
            ("collateral", "Collateral"),
            ("borrowed", "BYC borrowed"),
            ("emergency", "Emergency switch"),
        ):
            value = QLabel("-")
            value.setWordWrap(True)
            value.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            self._values[key] = value
            form.addRow(label, value)
        layout.addWidget(protocol)

        liquidations, box = self._section("Liquidations", ("vaults",), form=False)
        self._liquidation_summary = QLabel("-")
        self._liquidation_summary.setWordWrap(True)
        box.addWidget(self._liquidation_summary)
        box.addWidget(QLabel("Auctions running (the protocol sells the vault's XCH for BYC)"))
        self._auctions_running = _table(
            ("Vault", "XCH", "Debt (BYC)", "Price", "vs XCH price", "Ends", "Bids")
        )
        box.addWidget(self._auctions_running)
        box.addWidget(QLabel("Closest to liquidation"))
        self._closest = _table(
            ("Vault", "XCH", "Debt (BYC)", "Ratio", "Liquidation", "XCH drop", "State")
        )
        box.addWidget(self._closest)
        layout.addWidget(liquidations)

        treasury, box = self._section(
            "Treasury and auctions",
            ("treasury", "recharge_auctions", "surplus_auctions"),
            form=False,
        )
        self._treasury = QLabel("-")
        self._treasury.setWordWrap(True)
        box.addWidget(self._treasury)
        self._auctions = _table(("Auction", "Kind", "Status", "Can settle", "Detail"))
        box.addWidget(self._auctions)
        layout.addWidget(treasury)

        governance, box = self._section("Governance", ("bills",), form=False)
        self._bills = _table(("Coin", "CRT locked", "Bill", "Status", "Exit"))
        box.addWidget(self._bills)
        layout.addWidget(governance)

        announcers, box = self._section("Oracle price announcers", ("announcers",), form=False)
        self._announcers = _table(("Announcer", "Price", "State", "Price expiry", "Deposit"))
        box.addWidget(self._announcers)
        layout.addWidget(announcers)

        statutes, box = self._section("Statutes", ("statutes",), form=False)
        self._statutes = _table(("#", "Statute", "Value"))
        box.addWidget(self._statutes)
        layout.addWidget(statutes)
        layout.addStretch(1)

        self._round_timer = QTimer(self)
        self._round_timer.setInterval(ROUND_EVERY_MS)
        self._round_timer.timeout.connect(self.refresh)
        self._redraw_timer = QTimer(self)
        self._redraw_timer.setInterval(REDRAW_EVERY_MS)
        self._redraw_timer.timeout.connect(self._render)
        self._render()

    def _section(
        self, title: str, reads: Sequence[str], *, form: bool = True
    ) -> tuple[QGroupBox, Any]:
        group = QGroupBox(title)
        outer = QVBoxLayout(group)
        note = QLabel("")
        note.setWordWrap(True)
        note.setStyleSheet(f"color: {WARNING_YELLOW};")
        note.hide()
        outer.addWidget(note)
        for read in reads:
            self._notes.setdefault(read, []).append(note)
        if form:
            inner: Any = QFormLayout()
            inner.setHorizontalSpacing(16)
            outer.addLayout(inner)
            return group, inner
        return group, outer

    # -- the switch and the rounds ---------------------------------------------

    def set_enabled(self, enabled: bool) -> None:
        """Settings > Subsystems. Off stops asking at once; on asks when the page is open."""
        self._enabled = bool(enabled)
        if not self._enabled:
            self._round_timer.stop()
            self._redraw_timer.stop()
            self._round_due = False
            self._stop.set()  # a round under way ends at its next wait
        elif self.isVisible():
            self._opened()
        self._render()

    def is_enabled(self) -> bool:
        return self._enabled

    def fetching(self) -> bool:
        """A round of reads is under way."""
        return self._thread is not None

    def refresh(self) -> bool:
        """Start a round of reads now. False when switched off, closed, or one is under way."""
        if not self._enabled or not self.isVisible():
            # Closed, no new round starts: not from the timer, nor one that
            # was due once a round being stopped had ended.
            return False
        if self._thread is not None:
            if self._stop.is_set():
                self._round_due = True  # the round still stopping hands over to a new one
            return False
        if self._api is None:
            self._api = self._client_factory(self._stop)
        self._stop.clear()
        self._round_started_at = self._wall()
        self._round_seen = 0
        self._round_answered = 0
        self._round_why = ""
        thread = QThread(self)
        worker = _RoundWorker(self._api, self._stop, self._wall)
        worker.moveToThread(thread)
        thread.started.connect(worker.run)
        worker.result.connect(self._on_result)
        worker.done.connect(self._on_done)
        worker.done.connect(thread.quit)
        thread.finished.connect(worker.deleteLater)
        thread.finished.connect(self._on_thread_finished)
        thread.finished.connect(thread.deleteLater)
        self._thread = thread
        self._worker = worker
        thread.start()
        self._render()
        return True

    def stop_background_work(self, timeout_ms: int = STOP_WAIT_MS) -> None:
        """App closing: stop the round under way, waiting at most ``timeout_ms``.

        The page's thread stops at once, even mid-request: the request itself
        runs on a daemon thread of its own (:func:`poll.interruptible`), left
        to finish on its own. A QThread destroyed while it runs aborts the
        process (qFatal), so this waits for it.

        Should it still be running after the wait, it is never terminated:
        terminate() can land while the thread holds Python's lock, and the
        close then hangs for ever (permuto's live.py found this). It is
        detached from the page instead and kept, so that destroying the
        window does not destroy it. Were it still running when Python itself
        exits, Qt would abort the process there, after the window, the
        settings and the engine have all closed.
        """
        self._round_timer.stop()
        self._redraw_timer.stop()
        self._round_due = False
        self._stop.set()
        thread = self._thread
        if thread is None:
            return
        thread.quit()
        if not thread.wait(timeout_ms):
            _log.warning(
                "[Circuit] the page's thread did not stop in %d ms; it is left to end on its own",
                timeout_ms,
            )
            self._abandon(thread, self._worker)
        self._thread = None
        self._worker = None

    def _abandon(self, thread: QThread, worker: _RoundWorker | None) -> None:
        """Leave a thread that would not stop to end on its own, apart from the page."""
        links: list[tuple[Any, Any]] = [(thread.finished, self._on_thread_finished)]
        if worker is not None:
            links += [(worker.result, self._on_result), (worker.done, self._on_done)]
        for signal, slot in links:  # nothing it does later reaches the page
            try:
                signal.disconnect(slot)
            except (RuntimeError, TypeError):  # already gone
                pass
        thread.setParent(None)  # a child would die with the page, running
        _ABANDONED.append((thread, worker))
        # A later round has its own stop flag and client: clearing the old
        # flag would set the abandoned round going again.
        self._stop = threading.Event()
        self._api = None

    def showEvent(self, event: Any) -> None:  # noqa: N802 -- Qt's name
        super().showEvent(event)
        if self._enabled:
            self._opened()

    def hideEvent(self, event: Any) -> None:  # noqa: N802 -- Qt's name
        super().hideEvent(event)
        # A round under way finishes; no new one starts until the page is open again.
        self._round_timer.stop()
        self._redraw_timer.stop()

    def _opened(self) -> None:
        self._round_timer.start()
        self._redraw_timer.start()
        if not self._fresh():
            self.refresh()

    def _fresh(self) -> bool:
        """The last round started under FRESH_FOR_S ago and is not cut short."""
        started = self._round_started_at
        if started is None or self._wall() - started >= FRESH_FOR_S:
            return False
        if self._thread is not None:
            return not self._stop.is_set()  # running on, or being stopped
        return not self._round_why

    @Slot(object)
    def _on_result(self, result: poll.Result) -> None:
        self._round_seen += 1
        error = result.error
        if result.ok:
            # Checked before it replaces the last good answer: one of an
            # unexpected shape counts as a failed read, so the page keeps
            # the older answer (and, say, the emergency banner it carried)
            # beside a note, instead of emptying the section.
            try:
                model.section_from(
                    result.name, result.data, now=self._wall(), answered_at=result.at
                )
            except model.SHAPE_ERRORS as exc:
                error = model.shape_problem(exc)
        if error:
            self._errors[result.name] = error
        else:
            self._round_answered += 1
            self._answers[result.name] = result.data
            self._answered_at[result.name] = result.at
            self._errors.pop(result.name, None)
        self._render()

    @Slot(int, str)
    def _on_done(self, answered: int, why: str) -> None:
        self._round_ended_at = self._wall()
        if not why and self._round_seen < len(poll.PAGE_READS):
            # poll.fetch() ends a round between two reads once told to stop.
            why = "stopped"
        self._round_why = why
        self._render()

    @Slot()
    def _on_thread_finished(self) -> None:
        self._thread = None
        self._worker = None
        if self._round_due:
            self._round_due = False
            self.refresh()
        self._render()

    # -- drawing ----------------------------------------------------------------

    def _status_text(self) -> tuple[str, str]:
        if not self._enabled:
            return (
                "Switched off: nothing is asked. Settings > Advanced > Subsystems switches "
                "this page on.",
                TEXT_SECONDARY,
            )
        total = len(poll.PAGE_READS)
        now = self._wall()
        if self._thread is not None and not self._stop.is_set():
            return (
                f"Reading Circuit's API: {self._round_seen} of {total} answers so far.",
                TEXT_SECONDARY,
            )
        if self._round_started_at is None:
            return ("Not read yet: the page reads when it is open.", TEXT_SECONDARY)
        when = model.fmt_utc(self._round_ended_at or self._round_started_at, now)
        if self._round_why and self._round_why != "stopped":
            return (f"The last read stopped at {when}: {self._round_why}", WARNING_YELLOW)
        text = f"Read at {when}: {self._round_answered} of {total} answered."
        if self._round_why == "stopped":
            text += " (stopped before the end)"
        if self._errors:
            return (text + " Notes in the sections below.", WARNING_YELLOW)
        return (text + " The page reads again every 3 minutes while it is open.", TEXT_SECONDARY)

    def _render(self) -> None:
        now = self._wall()
        view = model.build_view(
            self._answers, now=now, errors=self._errors, answered_at=self._answered_at
        )
        text, colour = self._status_text()
        self._status.setText(text)
        self._status.setStyleSheet(f"color: {colour};")
        self._refresh_button.setEnabled(self._enabled and self._thread is None)
        lines: dict[int, tuple[QLabel, list[str]]] = {}
        for notes in self._notes.values():
            for note in notes:
                lines.setdefault(id(note), (note, []))
        for read, problem in view.problems.items():
            for note in self._notes.get(read, ()):
                lines[id(note)][1].append(f"{read}: {problem}")
        for note, text in lines.values():
            note.setText("\n".join(text))
            note.setVisible(bool(text))
        self._draw_protocol(view, now)
        self._draw_liquidations(view, now)
        self._draw_treasury(view, now)
        self._draw_governance(view)
        self._draw_announcers(view)
        self._draw_statutes(view)

    def _set(self, key: str, text: str) -> None:
        self._values[key].setText(text)

    def _draw_protocol(self, view: model.CircuitView, now: float) -> None:
        oracle, protocol, vaults = view.oracle, view.protocol, view.vaults
        banner = ""
        if oracle is None:
            self._set("price", "-")
            self._set("oracle", "-")
        else:
            point = oracle.protocol_price
            self._set(
                "price",
                "-"
                if point is None
                else f"{model.fmt_usd(point.usd)} per XCH, set at {model.fmt_utc(point.at, now)}",
            )
            if oracle.queued:
                newest, usable_at = oracle.queued[-1]
                when = (
                    "usable now"
                    if usable_at <= now
                    else "usable " + model.fmt_from_now(usable_at, now)
                )
                self._set(
                    "oracle",
                    f"newest price {model.fmt_usd(newest.usd)}, announced at "
                    f"{model.fmt_utc(newest.at, now)}, {when}",
                )
            elif oracle.matured is not None:
                self._set("oracle", f"no newer price than {model.fmt_usd(oracle.matured.usd)}")
            else:
                self._set("oracle", "no price yet")
        if protocol is None:
            for key in ("fee", "savings", "liquidation", "collateral", "emergency"):
                self._set(key, "-")
        else:
            self._set("fee", self._rate(protocol.stability_fee_apy, "charged on BYC borrowed"))
            self._set(
                "savings", self._rate(protocol.savings_apy, "paid on BYC deposited in savings")
            )
            parts = []
            if protocol.liquidation_ratio_pct is not None:
                parts.append(f"below {protocol.liquidation_ratio_pct:g}% collateral")
            if protocol.liquidation_penalty_pct is not None:
                parts.append(f"{protocol.liquidation_penalty_pct:g}% penalty")
            self._set("liquidation", "; ".join(parts) or "-")
            locked, cap = protocol.collateral_locked_xch, protocol.collateral_cap_xch
            if locked is None:
                self._set("collateral", "-")
            elif cap:
                self._set(
                    "collateral",
                    f"{model.fmt_units(locked)} XCH locked, of a {model.fmt_units(cap)} XCH cap "
                    f"({model.fmt_pct(locked / cap, 1)})",
                )
            else:
                self._set("collateral", f"{model.fmt_units(locked)} XCH locked")
            if protocol.emergency is None:
                self._set("emergency", "unknown: the statutes answer has no BLOCK_ISSUANCE")
            elif protocol.emergency:
                self._set("emergency", f"ON: BLOCK_ISSUANCE is {protocol.block_issuance}")
                banner = (
                    "Circuit's emergency switch is on (BLOCK_ISSUANCE = "
                    f"{protocol.block_issuance}): the protocol's core functions are switched off."
                )
            else:
                self._set("emergency", f"off (BLOCK_ISSUANCE is {protocol.block_issuance})")
            if protocol.status and protocol.status != "ok":
                banner = banner or f"The statutes answer says status {protocol.status!r}."
        if vaults is None:
            self._set("borrowed", "-")
        else:
            text = (
                f"{model.fmt_units(vaults.debt_byc)} BYC, in {vaults.with_debt} of "
                f"{vaults.count} vaults"
            )
            ratio = view.collateral_ratio()
            if ratio is not None:
                text += f"; all their collateral is worth {model.fmt_pct(ratio)} of it"
            self._set("borrowed", text)
        self._banner.setText(banner)
        self._banner.setVisible(bool(banner))

    @staticmethod
    def _rate(apy: float | None, what: str) -> str:
        return "-" if apy is None else f"{model.fmt_pct(apy, 2)} a year, {what}"

    def _draw_liquidations(self, view: model.CircuitView, now: float) -> None:
        vaults = view.vaults
        if vaults is None:
            self._liquidation_summary.setText("-")
            _fill(self._auctions_running, [], "Not read yet")
            _fill(self._closest, [], "Not read yet")
            return
        parts = [
            f"{len(vaults.in_auction)} auction{'' if len(vaults.in_auction) == 1 else 's'} running",
            f"{len(vaults.waiting)} liquidatable with no auction yet",
            f"{len(vaults.nearing)} nearing liquidation",
            f"{len(vaults.bad_debt)} in bad debt"
            + (f" ({model.fmt_units(vaults.bad_debt_byc)} BYC)" if vaults.bad_debt else ""),
        ]
        self._liquidation_summary.setText("; ".join(parts) + ".")
        price = view.price_usd
        rows = []
        for row in vaults.in_auction:
            share = (
                model.fmt_pct(row.auction_usd / price)
                if row.auction_usd is not None and price
                else "-"
            )
            rows.append(
                (
                    model.short_id(row.name),
                    model.fmt_units(row.collateral_xch),
                    model.fmt_units(row.debt_byc),
                    "-" if row.auction_usd is None else model.fmt_usd(row.auction_usd),
                    share,
                    "-"
                    if row.auction_ends_at is None
                    else model.fmt_from_now(row.auction_ends_at, now),
                    "open" if row.biddable else "not open",
                )
            )
        _fill(self._auctions_running, rows, "No liquidation auction is running")
        rows = []
        for row in (*vaults.waiting, *vaults.closest):
            drop = view.drop_to_liquidation(row)
            rows.append(
                (
                    model.short_id(row.name),
                    model.fmt_units(row.collateral_xch),
                    model.fmt_units(row.debt_byc),
                    "-" if row.ratio is None else model.fmt_pct(row.ratio),
                    "-" if row.liquidation_usd is None else model.fmt_usd(row.liquidation_usd),
                    "-" if drop is None else ("now" if drop <= 0 else model.fmt_pct(drop, 1)),
                    row.state,
                )
            )
        _fill(self._closest, rows, "No vault has debt")

    def _draw_treasury(self, view: model.CircuitView, now: float) -> None:
        treasury = view.treasury
        if treasury is None:
            self._treasury.setText("-")
        else:
            can = []
            can.append(
                "a surplus auction can start"
                if treasury.can_start_surplus
                else "no surplus auction can start"
            )
            can.append(
                "a recharge auction can start"
                if treasury.can_start_recharge
                else "no recharge auction can start"
            )
            ring = "" if treasury.is_ring else " (not a complete ring)"
            self._treasury.setText(
                f"The treasury holds {model.fmt_units(treasury.balance_byc)} BYC in "
                f"{treasury.coins} coins{ring}, and keeps between "
                f"{model.fmt_units(treasury.minimum_byc)} and "
                f"{model.fmt_units(treasury.maximum_byc)} BYC: {can[0]}, {can[1]}."
            )
        auctions = [*(view.recharge_auctions or ()), *(view.surplus_auctions or ())]
        rows = [
            (
                model.short_id(row.name),
                row.kind,
                row.status or "-",
                "yes" if row.can_be_settled else "no",
                row.detail,
            )
            for row in auctions
        ]
        empty = (
            "Not read yet"
            if view.recharge_auctions is None and view.surplus_auctions is None
            else "No auctions"
        )
        _fill(self._auctions, rows, empty)

    def _draw_governance(self, view: model.CircuitView) -> None:
        if view.bills is None:
            _fill(self._bills, [], "Not read yet")
            return
        rows = [
            (
                model.short_id(row.name),
                model.fmt_units(row.crt),
                row.proposal,
                row.status or "-",
                row.exit,
            )
            for row in view.bills
        ]
        _fill(self._bills, rows, "No governance coins")

    def _draw_announcers(self, view: model.CircuitView) -> None:
        if view.announcers is None:
            _fill(self._announcers, [], "Not read yet")
            return
        rows = [
            (
                model.short_id(row.name),
                model.fmt_usd(row.price_usd),
                row.state,
                row.expires,
                f"{model.fmt_units(row.deposit_xch)} XCH",
            )
            for row in view.announcers
        ]
        _fill(self._announcers, rows, "No announcers")

    def _draw_statutes(self, view: model.CircuitView) -> None:
        if view.protocol is None:
            _fill(self._statutes, [], "Not read yet")
            return
        rows = [(str(index), name, str(value)) for index, name, value in view.protocol.statutes]
        _fill(self._statutes, rows, "No statutes")
