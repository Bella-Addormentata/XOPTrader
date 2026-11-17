"""The report: what the book did over a period, read back from its database."""

from __future__ import annotations

import csv
from datetime import UTC, datetime

import pytest

from gui.services.options import report, store

T0 = int(datetime(2026, 11, 15, tzinfo=UTC).timestamp())
UNIT = 1000  # quote-asset mojos per unit (wUSDC.b has three decimals)
XCH = 10**12
USD = b"\x07" * 32
ME = b"\x33" * 32
CALL, PUT, OLD, LATER = (bytes([n]) * 32 for n in (0xA1, 0xB2, 0xC3, 0xD4))

OFF_PEG = "not quoting: spot outside the Dexie range"


def _quote(*, model: float, units: int, set_by: str) -> dict:
    return {
        "spot": 1.15,
        "sigma": 0.8,
        "strike": 1.25,
        "seconds_to_expiry": 86_400,
        "model": model,
        "carry": 0.0002,
        "markup": 0.0001,
        "floor": 0.01,
        "per_xch": units / UNIT / 0.5,
        "units": units,
        "set_by": set_by,
        "delta": 0.2,
        "lean": 0.0,
    }


def _contract(book: store.OptionStore, launcher: bytes, side: str, *, at: int, fee: int) -> None:
    """A 0.5 XCH contract (strike 1.25 for a call, 1.00 for a put), minted and confirmed."""
    call = side == "call"
    book.add_contract(
        launcher_id=launcher,
        slot=("C110@" if call else "P90@") + str(T0 + 86_400),
        side=side,
        underlying_asset=None if call else USD,
        underlying_amount=XCH // 2 if call else 500,
        strike_asset=USD if call else None,
        strike_amount=625 if call else XCH // 2,
        expiration=T0 + 86_400,
        creator_puzzle_hash=ME,
        bundle=b"mint",
        deadline=at + 600,
        fee=fee,
        now=at,
    )
    (mint,) = [row for row in book.broadcasts() if row.launcher_id == launcher]
    book.settle_broadcast(
        mint.id,
        "confirmed",
        contract_status=store.OPEN,
        event="collateral_locked",
        detail={"fee": fee},
        now=at + 100,
    )


def _offer(book: store.OptionStore, launcher: bytes, *, at: int, quote: dict | None) -> int:
    return book.add_offer(
        launcher_id=launcher,
        price_asset=USD,
        price_amount=quote["units"] if quote else 5,
        expires_at=None,
        bundle=b"offer",
        offer_text=f"offer{at}",
        quote=quote,
        now=at,
    )


def _book() -> tuple[store.OptionStore, dict[str, int]]:
    """Three hours of a small book, from T0.

    The call is minted, offered twice (once at the floor, once by the model),
    sold from the second offer, then clawed back after expiry. The put is
    minted and offered at the floor and is still open. One older contract was
    cancelled before the period and one is minted after it.
    """
    book = store.OptionStore(":memory:")
    _contract(book, OLD, "call", at=T0 - 5000, fee=100_000_000)
    book.set_status(
        OLD, store.CLOSED, event="collateral_returned", detail={"by": store.CANCEL}, now=T0 - 1000
    )

    _contract(book, CALL, "call", at=T0 + 100, fee=300_000_000)
    first = _offer(book, CALL, at=T0 + 300, quote=_quote(model=0.004, units=5, set_by="floor"))
    book.set_offer(first, desk_offer_id="d-1")
    taken = _offer(book, CALL, at=T0 + 1200, quote=_quote(model=0.012, units=7, set_by="model"))
    book.set_status(
        CALL,
        store.SOLD,
        event="sold",
        detail={"asset": USD.hex(), "amount": 7, "offer_id": taken},
        taken_offer=taken,
        now=T0 + 3000,
    )
    clawback = book.add_broadcast(
        launcher_id=CALL, kind=store.CLAWBACK, bundle=b"claw", deadline=T0 + 8000, now=T0 + 7000
    )
    book.settle_broadcast(
        clawback,
        "confirmed",
        contract_status=store.CLOSED,
        event="collateral_returned",
        detail={"by": store.CLAWBACK, "fee": 0},
        now=T0 + 7200,
    )

    _contract(book, PUT, "put", at=T0 + 400, fee=200_000_000)
    _offer(book, PUT, at=T0 + 600, quote=_quote(model=0.003, units=5, set_by="floor"))

    _contract(book, LATER, "call", at=T0 + 20_000, fee=100_000_000)

    # The step journal: quoting, held back by the range check, quoting, then
    # the book stopped for a while and came back to a wallet that was syncing.
    for at, verdict in (
        (T0, store.QUOTING),
        (T0 + 900, store.QUOTING),
        (T0 + 1800, OFF_PEG),
        (T0 + 2700, OFF_PEG),
        (T0 + 3600, store.QUOTING),
        (T0 + 9000, "wallet not ready"),
    ):
        book.log_step(verdict, now=at, force=True)
    return book, {"first": first, "taken": taken}


UNTIL = T0 + 3 * 3600


# -- the period -------------------------------------------------------------


@pytest.mark.parametrize(
    ("text", "since"),
    [
        ("24h", T0 - 86_400),
        ("90m", T0 - 5400),
        ("1.5h", T0 - 5400),
        ("7d", T0 - 7 * 86_400),
        ("2w", T0 - 14 * 86_400),
        (" 24H ", T0 - 86_400),
        ("all", 0),
        ("2026-11-14", T0 - 86_400),
        ("2026-11-14T12:00", T0 - 43_200),  # no zone: UTC
        ("2026-11-14T12:00Z", T0 - 43_200),
        ("2026-11-14T13:00+01:00", T0 - 43_200),
    ],
)
def test_a_period_starts_where_it_says(text: str, since: int) -> None:
    assert report.parse_since(text, now=T0) == since


@pytest.mark.parametrize("text", ["", "yesterday", "24", "h24", "-3h", "24 hours"])
def test_a_period_that_cannot_be_read_is_refused_with_examples(text: str) -> None:
    with pytest.raises(report.ReportError, match="24h or 7d"):
        report.parse_since(text, now=T0)


# -- how long the book quoted, and what held it back -----------------------


def test_the_journal_says_how_long_the_book_quoted_and_what_held_it_back() -> None:
    book, _ = _book()

    summary = report.summarize(book, UNIT, T0, UNTIL)

    # Each row speaks until the next, but for no longer than the book can go
    # without writing one: from T0+3600 it covers 20 minutes, not 90.
    assert summary.quoting_seconds == 900 + 900 + 1200
    assert summary.held_back == {OFF_PEG: (1800, 1), "wallet not ready": (1200, 1)}
    assert summary.journal_seconds == 3000 + 1800 + 1200  # the rest it was not running


def test_a_period_that_starts_inside_a_spell_counts_its_part_of_it() -> None:
    book, _ = _book()

    summary = report.summarize(book, UNIT, T0 + 2000, UNTIL)

    # The row at T0+1800 runs into the period: 700 s of it fall inside.
    assert summary.held_back[OFF_PEG] == (700 + 900, 1)
    assert summary.quoting_seconds == 1200


def test_a_verdict_that_comes_back_is_another_spell() -> None:
    book = store.OptionStore(":memory:")
    for at, verdict in (
        (T0, OFF_PEG),
        (T0 + 100, store.QUOTING),
        (T0 + 200, OFF_PEG),
        (T0 + 300, store.QUOTING),
        (T0 + 5000, store.QUOTING),
    ):
        book.log_step(verdict, now=at, force=True)
    book.log_step(OFF_PEG, now=T0 + 5100, force=True)
    book.log_step(OFF_PEG, now=T0 + 9000, force=True)  # the same verdict, after a stop

    summary = report.summarize(book, UNIT, T0, T0 + 10_000)

    seconds, spells = summary.held_back[OFF_PEG]
    assert spells == 4
    assert seconds == 100 + 100 + 1200 + 1000


def test_steps_half_an_hour_apart_count_as_running_when_the_journal_says_so() -> None:
    # Every step writes a row when steps are more than 15 minutes apart. Rows
    # 30 minutes apart are a running book if that was its interval, and a
    # book that stopped in between if it was not.
    timed, untimed = store.OptionStore(":memory:"), store.OptionStore(":memory:")
    for at in range(T0, T0 + 3 * 3600, 1800):
        timed.log_step(store.QUOTING, interval=1800, now=at, force=True)
        untimed.log_step(store.QUOTING, now=at, force=True)

    assert report.summarize(timed, UNIT, T0, UNTIL).quoting_seconds == 3 * 3600
    assert report.summarize(untimed, UNIT, T0, UNTIL).quoting_seconds == 6 * 1200


def test_a_step_that_keeps_failing_is_one_spell() -> None:
    # Each failed step journals its market as quoting, then the failure, with
    # the same time. The quoting rows cover nothing and split nothing.
    book = store.OptionStore(":memory:")
    for at in range(T0, T0 + 600, 60):
        book.log_step(store.QUOTING, interval=60, now=at, force=True)
        book.log_step("step failed", detail="RuntimeError: boom", interval=60, now=at, force=True)

    summary = report.summarize(book, UNIT, T0, T0 + 600)

    assert summary.quoting_seconds == 0
    assert summary.held_back == {"step failed": (600, 1)}


def test_a_period_before_the_journal_existed_says_so() -> None:
    book = store.OptionStore(":memory:")

    summary = report.summarize(book, UNIT, T0, UNTIL)
    text = report.format_text(summary)

    assert (summary.journal_seconds, summary.quoting_seconds, summary.held_back) == (0, 0, {})
    assert "Quoting: nothing in the step journal for this period" in text
    assert "Offers: 0 made, 0 listed on the site" in text
    assert "0 sold, 0 exercised, 0 clawed back, 0 cancelled" in text


# -- contracts: premium, model value at sale, fees, locked collateral ------


def test_each_contract_shows_its_offers_sale_fees_and_locked_time() -> None:
    book, _ = _book()

    summary = report.summarize(book, UNIT, T0, UNTIL)

    by_id = {line.launcher_id: line for line in summary.contracts}
    assert set(by_id) == {CALL.hex(), PUT.hex()}  # not the one closed before, nor the one after
    call, put = by_id[CALL.hex()], by_id[PUT.hex()]
    assert (call.side, call.size_xch, call.strike, call.status) == ("call", 0.5, 1.25, store.CLOSED)
    assert (call.offers, call.listed, call.quoted, call.on_floor) == (2, 1, 2, 1)
    assert (call.premium, call.premium_in_xch) == (pytest.approx(0.007), False)
    assert call.model_at_sale == pytest.approx(0.012 * 0.5)  # the offer that was taken
    assert call.fees_mojos == 300_000_000  # the mint; the clawback paid nothing
    assert call.locked_hours == pytest.approx((7200 - 200) / 3600)

    assert (put.side, put.size_xch, put.strike, put.status) == ("put", 0.5, 1.0, store.OPEN)
    assert (put.offers, put.listed, put.quoted, put.on_floor) == (1, 0, 1, 1)
    assert (put.premium, put.model_at_sale) == (0.0, None)
    assert put.fees_mojos == 200_000_000
    assert put.locked_hours == pytest.approx((UNTIL - T0 - 500) / 3600)  # still locked

    assert (summary.offers, summary.listed, summary.quoted, summary.on_floor) == (3, 1, 3, 2)
    assert (summary.sales, summary.exercises, summary.clawbacks, summary.cancels) == (1, 0, 1, 0)
    assert summary.fees_mojos == 500_000_000
    assert summary.premium == pytest.approx(0.007)
    assert summary.locked_xch_hours == pytest.approx(
        0.5 * (7000 / 3600) + 0.5 * ((UNTIL - T0 - 500) / 3600)
    )


def test_a_period_counts_only_what_happened_in_it() -> None:
    book, _ = _book()

    summary = report.summarize(book, UNIT, T0 + 3600, UNTIL)

    call = {line.launcher_id: line for line in summary.contracts}[CALL.hex()]
    assert (call.offers, call.premium, call.fees_mojos) == (0, 0.0, 0)  # all before the period
    assert call.locked_hours == pytest.approx((7200 - 3600) / 3600)
    assert (summary.sales, summary.clawbacks, summary.offers) == (0, 1, 0)


def test_a_sale_in_the_period_finds_the_quote_of_an_offer_made_before_it() -> None:
    book, _ = _book()

    # The call's second offer was made at T0 + 1200 and taken at T0 + 3000.
    summary = report.summarize(book, UNIT, T0 + 2000, UNTIL)

    call = {line.launcher_id: line for line in summary.contracts}[CALL.hex()]
    assert call.premium == pytest.approx(0.007)
    assert call.model_at_sale == pytest.approx(0.012 * 0.5)
    assert (call.offers, call.quoted, summary.quoted) == (0, 0, 0)  # made before the period


def test_the_live_test_s_contract_has_its_strike_in_xch() -> None:
    # XCH on both legs: 0.0001 XCH for 0.0001 XCH is a strike of 1, not 1e9.
    book = store.OptionStore(":memory:")
    book.add_contract(
        launcher_id=CALL,
        slot="live-test",
        side="call",
        underlying_asset=None,
        underlying_amount=XCH // 10_000,
        strike_asset=None,
        strike_amount=XCH // 10_000,
        expiration=T0 + 86_400,
        creator_puzzle_hash=ME,
        bundle=b"mint",
        deadline=T0 + 600,
        now=T0,
    )

    (line,) = report.summarize(book, UNIT, T0, UNTIL).contracts

    assert (line.size_xch, line.strike) == (pytest.approx(0.0001), pytest.approx(1.0))


def test_a_premium_paid_in_xch_is_kept_apart_from_the_quote_asset_total() -> None:
    book = store.OptionStore(":memory:")
    _contract(book, CALL, "call", at=T0, fee=0)
    book.set_status(
        CALL, store.SOLD, event="sold", detail={"asset": None, "amount": XCH // 100}, now=T0 + 60
    )

    summary = report.summarize(book, UNIT, T0, UNTIL)

    (line,) = summary.contracts
    assert (line.premium, line.premium_in_xch, line.model_at_sale) == (0.01, True, None)
    assert summary.premium == 0.0
    assert "0.0100 XCH" in report.format_text(summary)


# -- what the terminal and the spreadsheet get ------------------------------


def test_the_text_report_reads_as_the_period_went() -> None:
    book, _ = _book()

    text = report.format_text(report.summarize(book, UNIT, T0, UNTIL))

    assert text.startswith("Options book, 2026-11-15 00:00 to 2026-11-15 03:00 UTC (3.0 h)")
    assert "Quoting: 0.8 h of the 1.7 h the book was running (50%)" in text
    held = [line.strip() for line in text.splitlines() if "held back" in line]
    assert held == [  # longest first
        f"held back, {OFF_PEG}: 0.5 h in 1 spell",
        "held back, wallet not ready: 0.3 h in 1 spell",
    ]
    assert "not running: 1.3 h" in text
    assert "Offers: 3 made, 1 listed on the site; 2 of 3 priced at the floor (min_premium)" in text
    rows = [line for line in text.splitlines() if line.startswith(("C110@", "P90@"))]
    assert len(rows) == 2
    assert rows[0].split()[:4] == [f"C110@{T0 + 86_400}", "closed", "0.5", "1.25"]
    assert "0.0070" in rows[0] and "0.0060" in rows[0] and "0.000300" in rows[0]
    assert "Totals: premium 0.007 (quote asset), fees 0.000500 XCH" in text
    assert "1 sold, 0 exercised, 1 clawed back, 0 cancelled" in text


def test_the_csv_has_a_row_per_contract(tmp_path) -> None:
    book, _ = _book()
    path = tmp_path / "options.csv"

    written = report.write_csv(report.summarize(book, UNIT, T0, UNTIL), path)

    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        rows = list(reader)
    assert written == 2
    assert tuple(reader.fieldnames or ()) == report._CSV_FIELDS
    call, put = rows
    assert (call["launcher_id"], call["side"], call["status"]) == (CALL.hex(), "call", "closed")
    assert float(call["premium"]) == pytest.approx(0.007) and call["premium_asset"] == "quote"
    assert float(call["model_at_sale"]) == pytest.approx(0.006)
    assert float(call["fees_xch"]) == pytest.approx(0.0003)
    assert call["expiration_utc"] == "2026-11-16 00:00"
    assert (put["model_at_sale"], put["on_floor"], put["quoted"]) == ("", "1", "1")
