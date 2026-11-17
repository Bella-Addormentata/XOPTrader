"""What the options book did over a period, read from its own database.

``python -m gui.services.options report --since 24h`` prints, for the period:

* how long the book quoted, what held it back and for how long, and how long
  it was not running at all (from the step journal);
* the offers it made, how many the site listed, and how many were priced by
  the model rather than the minimum premium (from the quotes journal);
* per contract, the premium a sale brought against the model value of the
  offer that was taken, the fees its transactions paid, and how long its
  collateral was locked (from contracts, broadcasts and events). A fee counts
  in the period its transaction was sent, once it has confirmed, so each fee
  falls in exactly one of two adjoining periods.

``--csv PATH`` writes the per-contract figures for a spreadsheet. The report
only reads, so it is safe to run while the book is running.

The journals start with the release that added them, so a period from before
it shows contracts, fees and sales but no quoting time and no price breakdown.
"""

from __future__ import annotations

import csv
import dataclasses
import re
import time
from collections.abc import Iterable
from datetime import UTC
from pathlib import Path

from gui.services.options import store
from gui.services.options.config import XCH_MOJOS

#: Longer than this between two journal rows means the book was not running in
#: between: while it runs it writes at least every ``JOURNAL_EVERY_SECONDS``
#: (on the first step after that), or every step when steps are further apart
#: than that. A row says how far apart they were meant to be; this is the slack.
_STEP_SLACK_SECONDS = 300
_RUNNING_GAP_SECONDS = store.JOURNAL_EVERY_SECONDS + _STEP_SLACK_SECONDS

_SPAN = re.compile(r"^\s*(\d+(?:\.\d+)?)\s*([mhdw])\s*$", re.IGNORECASE)
_SPAN_SECONDS = {"m": 60, "h": 3600, "d": 86400, "w": 7 * 86400}


class ReportError(ValueError):
    """The period asked for could not be understood."""


def parse_since(text: str, now: float) -> int:
    """The start of the period: ``24h``, ``7d``, ``90m``, ``2w``, an ISO date or time, or ``all``.

    A date or time without a zone is taken as UTC.
    """
    text = text.strip()
    if text.lower() == "all":
        return 0
    span = _SPAN.match(text)
    if span:
        return int(now - float(span.group(1)) * _SPAN_SECONDS[span.group(2).lower()])
    from datetime import datetime

    try:
        when = datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ReportError(
            f"--since {text!r}: give a span like 24h or 7d, a date like 2026-11-15, or 'all'"
        ) from exc
    if when.tzinfo is None:
        when = when.replace(tzinfo=UTC)
    return int(when.timestamp())


@dataclasses.dataclass(frozen=True)
class ContractLine:
    """One contract's part in the period."""

    launcher_id: str
    slot: str
    side: str
    size_xch: float
    strike: float  # quote asset per XCH
    expiration: int
    status: str  # now, not at the end of the period
    offers: int
    listed: int
    on_floor: int  # of its offers with a recorded quote, how many sat on the floor
    quoted: int  # of its offers, how many have a recorded quote
    premium: float  # from a sale in the period, in the quote asset (XCH if it was paid in XCH)
    premium_in_xch: bool
    model_at_sale: float | None  # the taken offer's model value for the whole contract
    fees_mojos: int  # what its confirmed transactions sent in the period paid
    locked_hours: float  # collateral locked within the period


@dataclasses.dataclass(frozen=True)
class Summary:
    since: int
    until: int
    journal_seconds: int  # the part of the period the step journal covers
    quoting_seconds: int
    held_back: dict[str, tuple[int, int]]  # verdict -> (seconds, spells)
    offers: int
    listed: int
    on_floor: int
    quoted: int
    contracts: list[ContractLine]
    sales: int
    exercises: int
    clawbacks: int
    cancels: int

    @property
    def fees_mojos(self) -> int:
        return sum(line.fees_mojos for line in self.contracts)

    @property
    def premium(self) -> float:
        return sum(line.premium for line in self.contracts if not line.premium_in_xch)

    @property
    def locked_xch_hours(self) -> float:
        return sum(line.locked_hours * line.size_xch for line in self.contracts)


def _overlap(start: float, end: float, since: float, until: float) -> float:
    return max(0.0, min(end, until) - max(start, since))


def _journal(rows: list[store.StepLogRow], since: int, until: int) -> tuple[int, int, dict]:
    """Seconds covered, seconds quoting, and the held-back verdicts with their spells."""
    covered = quoting = 0
    held: dict[str, list[int]] = {}
    previous: str | None = None  # the verdict running into this row, if it ran right up to it
    for index, row in enumerate(rows):
        after = rows[index + 1].at if index + 1 < len(rows) else until
        if row.at >= after:
            # Followed at once by another row, as a step that failed after it
            # had journaled its market: it covers no time, and breaks no spell.
            continue
        # A row speaks for the time until the next one, unless the gap is so
        # long that the book cannot have been running through it.
        gap = max(_RUNNING_GAP_SECONDS, (row.interval or 0) + _STEP_SLACK_SECONDS)
        end = min(after, row.at + gap)
        seconds = int(_overlap(row.at, end, since, until))
        if seconds > 0:
            covered += seconds
            if row.verdict == store.QUOTING:
                quoting += seconds
            else:
                entry = held.setdefault(row.verdict, [0, 0])
                entry[0] += seconds
                if previous != row.verdict:
                    entry[1] += 1  # a new spell: after another verdict, or after a gap
        previous = row.verdict if end >= after else None
    return covered, quoting, {verdict: (secs, spells) for verdict, (secs, spells) in held.items()}


def summarize(
    book_store: store.OptionStore, quote_unit_mojos: int, since: int, until: int
) -> Summary:
    """Everything the report says about ``[since, until]``."""
    rows = book_store.steps(since, until)
    before = book_store.last_step(before=since)
    if before is not None and (not rows or rows[0].id != before.id):
        rows = [before, *rows]
    covered, quoting, held = _journal(rows, since, until)

    # A sale in the period can take an offer made before it: its quote is
    # looked up among them all. The counts are of offers made in the period.
    every_quote = book_store.quotes()
    quotes = {offer_id: row for offer_id, row in every_quote.items() if row.at >= since}
    offers = [row for row in book_store.offers() if since <= row.created_at <= until]
    events = [event for event in book_store.events() if since <= event.at <= until]
    every_event = book_store.events()
    broadcasts = [
        row
        for row in book_store.broadcasts(None)
        if row.status == "confirmed" and since <= row.created_at <= until
    ]

    lines: list[ContractLine] = []
    for contract in book_store.contracts():
        if contract.created_at > until:
            continue
        if contract.status not in store.ACTIVE and contract.updated_at < since:
            continue
        own_offers = [row for row in offers if row.launcher_id == contract.launcher_id]
        own_quotes = [quotes[row.id] for row in own_offers if row.id in quotes]
        size_xch = (
            contract.underlying_amount if contract.side == "call" else contract.strike_amount
        ) / XCH_MOJOS
        call = contract.side == "call"
        quote_leg = contract.strike_amount if call else contract.underlying_amount
        # The live test's contract is XCH on both legs, so its strike is in XCH.
        quote_asset = contract.strike_asset if call else contract.underlying_asset
        unit = XCH_MOJOS if quote_asset is None else quote_unit_mojos
        strike = (quote_leg / unit) / size_xch if size_xch else 0.0

        premium, in_xch, model_at_sale = 0.0, False, None
        for event in events:
            if event.launcher_id != contract.launcher_id or event.kind != "sold":
                continue
            amount = int(event.detail.get("amount") or 0)
            in_xch = event.detail.get("asset") is None
            premium += amount / (XCH_MOJOS if in_xch else quote_unit_mojos)
            taken = every_quote.get(int(event.detail.get("offer_id") or 0))
            if taken is not None:
                model_at_sale = taken.model * size_xch

        locked_from: int | None = None
        locked_hours = 0.0
        for event in every_event:
            if event.launcher_id != contract.launcher_id:
                continue
            if event.kind == "collateral_locked":
                locked_from = event.at
            elif event.kind in ("collateral_returned", "exercised") and locked_from is not None:
                locked_hours += _overlap(locked_from, event.at, since, until) / 3600.0
                locked_from = None
        if locked_from is not None:
            locked_hours += _overlap(locked_from, until, since, until) / 3600.0

        lines.append(
            ContractLine(
                launcher_id=contract.launcher_id.hex(),
                slot=contract.slot,
                side=contract.side,
                size_xch=size_xch,
                strike=strike,
                expiration=contract.expiration,
                status=contract.status,
                offers=len(own_offers),
                listed=sum(row.desk_offer_id is not None for row in own_offers),
                on_floor=sum(row.set_by == "floor" for row in own_quotes),
                quoted=len(own_quotes),
                premium=premium,
                premium_in_xch=in_xch,
                model_at_sale=model_at_sale,
                fees_mojos=sum(
                    int(row.fee or 0)
                    for row in broadcasts
                    if row.launcher_id == contract.launcher_id
                ),
                locked_hours=locked_hours,
            )
        )

    returned = [event for event in events if event.kind == "collateral_returned"]
    return Summary(
        since=since,
        until=until,
        journal_seconds=covered,
        quoting_seconds=quoting,
        held_back=held,
        offers=len(offers),
        listed=sum(row.desk_offer_id is not None for row in offers),
        on_floor=sum(quote.set_by == "floor" for quote in quotes.values() if quote.at <= until),
        quoted=sum(1 for quote in quotes.values() if quote.at <= until),
        contracts=lines,
        sales=sum(event.kind == "sold" for event in events),
        exercises=sum(event.kind == "exercised" for event in events),
        clawbacks=sum(event.detail.get("by") == store.CLAWBACK for event in returned),
        cancels=sum(event.detail.get("by") == store.CANCEL for event in returned),
    )


def _when(at: float) -> str:
    return time.strftime("%Y-%m-%d %H:%M", time.gmtime(at))


def _hours(seconds: float) -> str:
    return f"{seconds / 3600:.1f} h"


def format_text(summary: Summary) -> str:
    """The report as the terminal shows it."""
    span = summary.until - summary.since
    out = [
        f"Options book, {_when(summary.since)} to {_when(summary.until)} UTC ({_hours(span)})",
        "",
    ]
    if summary.journal_seconds:
        share = summary.quoting_seconds / summary.journal_seconds * 100
        out.append(
            f"Quoting: {_hours(summary.quoting_seconds)} of the "
            f"{_hours(summary.journal_seconds)} the book was running ({share:.0f}%)"
        )
        for verdict, (seconds, spells) in sorted(
            summary.held_back.items(), key=lambda item: -item[1][0]
        ):
            plural = "" if spells == 1 else "s"
            out.append(f"  held back, {verdict}: {_hours(seconds)} in {spells} spell{plural}")
        idle = span - summary.journal_seconds
        if idle > 0:
            out.append(f"  not running: {_hours(idle)}")
    else:
        out.append("Quoting: nothing in the step journal for this period")
    floor = (
        f"; {summary.on_floor} of {summary.quoted} priced at the floor (min_premium)"
        if summary.quoted
        else ""
    )
    out.append(f"Offers: {summary.offers} made, {summary.listed} listed on the site{floor}")
    out.append("")
    if summary.contracts:
        out.append(
            f"{'contract':<20} {'status':<9} {'size':>8} {'strike':>7} {'expires (UTC)':<16} "
            f"{'offers':>6} {'premium':>9} {'model':>9} {'fees XCH':>10} {'locked':>8}"
        )
        for line in summary.contracts:
            model = "-" if line.model_at_sale is None else f"{line.model_at_sale:.4f}"
            premium = f"{line.premium:.4f}" + (" XCH" if line.premium_in_xch else "")
            out.append(
                f"{line.slot:<20} {line.status:<9} {line.size_xch:>8g} {line.strike:>7.4g} "
                f"{_when(line.expiration):<16} {line.offers:>6} {premium:>9} {model:>9} "
                f"{line.fees_mojos / XCH_MOJOS:>10.6f} {_hours(line.locked_hours * 3600):>8}"
            )
        out.append("")
    out.append(
        f"Totals: premium {summary.premium:g} (quote asset), fees "
        f"{summary.fees_mojos / XCH_MOJOS:.6f} XCH, collateral locked "
        f"{summary.locked_xch_hours:.1f} XCH-hours"
    )
    out.append(
        f"        {summary.sales} sold, {summary.exercises} exercised, "
        f"{summary.clawbacks} clawed back, {summary.cancels} cancelled"
    )
    return "\n".join(out)


_CSV_FIELDS = (
    "launcher_id",
    "slot",
    "side",
    "size_xch",
    "strike",
    "expiration_utc",
    "status",
    "offers",
    "listed",
    "quoted",
    "on_floor",
    "premium",
    "premium_asset",
    "model_at_sale",
    "fees_xch",
    "locked_hours",
)


def write_csv(summary: Summary, path: str | Path) -> int:
    """The per-contract figures as CSV. Returns how many rows were written."""
    rows: Iterable[dict] = (
        {
            "launcher_id": line.launcher_id,
            "slot": line.slot,
            "side": line.side,
            "size_xch": line.size_xch,
            "strike": round(line.strike, 9),
            "expiration_utc": _when(line.expiration),
            "status": line.status,
            "offers": line.offers,
            "listed": line.listed,
            "quoted": line.quoted,
            "on_floor": line.on_floor,
            "premium": line.premium,
            "premium_asset": "XCH" if line.premium_in_xch else "quote",
            "model_at_sale": "" if line.model_at_sale is None else line.model_at_sale,
            "fees_xch": line.fees_mojos / XCH_MOJOS,
            "locked_hours": round(line.locked_hours, 4),
        }
        for line in summary.contracts
    )
    count = 0
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=_CSV_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
            count += 1
    return count
