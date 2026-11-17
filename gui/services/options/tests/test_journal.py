"""What the book keeps about why it did what it did.

A run is reviewed after the fact from the database, not from terminal output:
the step journal says what each step saw and made of it, every offer keeps how
its price was worked out, and the events keep what each transaction paid and
which offer a sale came from. These tests step a :class:`test_service.Rig`
and read those records back.
"""

from __future__ import annotations

import logging
import sqlite3
from typing import Any

import pytest

pytest.importorskip("chia_wallet_sdk")

from gui.services.options import chain, market, store  # noqa: E402
from gui.services.options.service import STEP_FAILED, WALLET_NOT_READY  # noqa: E402
from gui.services.options.tests.test_service import Rig, _auto_fee, _by_side  # noqa: E402


def _offering(reports: list[Any]) -> list[str]:
    return [action for report in reports for action in report.actions if "offering" in action]


def _stale(rig: Rig, as_of: int) -> None:
    rig.read_market = lambda: market.Market(  # type: ignore[method-assign]
        spot=1.10, sigma=0.86, bid=1.078, ask=1.122, as_of=as_of
    )
    rig.book = rig.make_book()


# ---------------------------------------------------------------------------
# The step journal
# ---------------------------------------------------------------------------


def test_a_step_that_quotes_journals_the_market_it_quoted_from() -> None:
    rig = Rig()

    rig.steps(1)

    (row,) = rig.store.steps()
    assert row.verdict == store.QUOTING
    assert (row.spot, row.sigma) == (1.10, 0.86)
    assert (row.bid, row.ask) == (pytest.approx(1.10 * 0.98), pytest.approx(1.10 * 1.02))
    assert row.spot_at is not None and abs(row.spot_at - row.at) < 60


def test_a_book_stepped_on_a_timer_journals_its_interval() -> None:
    # So the report can tell steps half an hour apart from a book that stopped.
    rig = Rig()
    rig.steps(1)
    rig.book.step_interval = 1800.0
    rig.spot = 1.10 * (1 + 2 * store.JOURNAL_MOVE)
    rig.steps(1)

    assert [row.interval for row in rig.store.steps()] == [None, 1800]


def test_the_journal_keeps_news_not_every_step() -> None:
    rig = Rig()
    rig.steps(4)  # the same market throughout: one row says it all
    assert len(rig.store.steps()) == 1

    rig.spot = 1.10 * (1 + 2 * store.JOURNAL_MOVE)
    rig.steps(1)

    assert [row.spot for row in rig.store.steps()] == [1.10, pytest.approx(rig.spot)]


def test_a_step_that_cannot_quote_journals_why_once_while_the_reason_holds() -> None:
    rig = Rig()
    _stale(rig, as_of=rig.world.now() - 4000)

    for _ in range(3):
        rig.steps(1)
        rig.world.advance(60)  # the message changes ("4060 s old"); the verdict does not

    (row,) = rig.store.steps()
    assert row.verdict == f"not quoting: {market.STALE}"
    assert row.detail.startswith("the spot price is 4") and row.detail.endswith(" s old")
    assert row.spot == 1.10  # what it was judged by
    assert rig.contracts() == []


def test_a_refusal_keeps_the_prices_it_was_judged_by() -> None:
    rig = Rig()
    rig.read_market = lambda: market.Market(  # type: ignore[method-assign]
        spot=1.10, sigma=0.86, bid=1.20, ask=1.25, as_of=rig.world.now()
    )
    rig.book = rig.make_book()

    rig.steps(1)

    (row,) = rig.store.steps()
    assert row.verdict == f"not quoting: {market.OFF_PEG}"
    assert "outside the executable range" in row.detail
    assert (row.spot, row.bid, row.ask) == (1.10, 1.20, 1.25)


def test_a_market_that_cannot_be_read_is_journaled_without_prices() -> None:
    rig = Rig()
    rig.market_error = "CoinGecko did not answer"

    rig.steps(1)

    (row,) = rig.store.steps()
    assert (row.verdict, row.detail) == (f"not quoting: {market.NO_DATA}", rig.market_error)
    assert (row.spot, row.sigma, row.bid, row.ask, row.spot_at) == (None,) * 5


def test_a_wallet_that_is_not_ready_is_journaled_with_its_reason() -> None:
    rig = Rig()
    rig.wallet.unready = "wallet is syncing"

    rig.steps(3)

    (row,) = rig.store.steps()
    assert (row.verdict, row.detail, row.spot) == (WALLET_NOT_READY, "wallet is syncing", None)

    rig.wallet.unready = None
    rig.steps(1)
    assert [row.verdict for row in rig.store.steps()] == [WALLET_NOT_READY, store.QUOTING]


def test_a_journal_that_cannot_be_written_does_not_stop_the_book(monkeypatch, caplog) -> None:
    rig = Rig()

    def full(*args: Any, **kwargs: Any) -> bool:
        raise sqlite3.OperationalError("database or disk is full")

    monkeypatch.setattr(rig.store, "log_step", full)
    with caplog.at_level(logging.WARNING, logger="gui.services.options.service"):
        rig.steps(4)

    assert len(rig.contracts(store.OPEN)) == 2
    assert len(rig.store.offers(status="live")) == 2
    assert "the step journal could not be written: database or disk is full" in caplog.text


# ---------------------------------------------------------------------------
# How each price was worked out
# ---------------------------------------------------------------------------


def test_every_offer_keeps_how_its_price_was_worked_out() -> None:
    rig = Rig()

    rig.steps(4)

    offers = rig.store.offers()
    quotes = rig.store.quotes()
    assert len(offers) == 2
    assert sorted(quotes) == sorted(offer.id for offer in offers)
    for offer in offers:
        quote = quotes[offer.id]
        assert (quote.launcher_id, quote.units, quote.at) == (
            offer.launcher_id,
            offer.price_amount,
            offer.created_at,
        )
        assert (quote.spot, quote.sigma) == (1.10, 0.86)
        assert quote.model > 0 and quote.seconds_to_expiry > 0
        assert quote.per_xch >= max(quote.model + quote.carry, quote.floor)
        assert quote.set_by == "model"


def test_an_offer_set_by_the_model_says_so() -> None:
    rig = Rig()

    offering = _offering(rig.steps(4))

    assert len(offering) == 2
    for action in offering:
        assert " (model value " in action and "per XCH)" in action
        assert "on the floor" not in action


def test_an_offer_on_the_floor_says_so_and_still_keeps_the_model_value() -> None:
    rig = Rig(pricing={"min_premium": 5.0})  # far above anything the model makes of these

    offering = _offering(rig.steps(4))

    assert len(offering) == 2
    assert all("(on the floor; model value " in action for action in offering)
    quotes = list(rig.store.quotes().values())
    assert {quote.set_by for quote in quotes} == {"floor"}
    assert all(quote.per_xch == pytest.approx(5.0) and 0 < quote.model < 0.1 for quote in quotes)


# ---------------------------------------------------------------------------
# What was paid, and where a sale came from
# ---------------------------------------------------------------------------


def test_the_fee_a_mint_paid_is_kept_with_its_event() -> None:
    rig = _auto_fee(put_strikes_pct=[])
    rig.wallet.rate = 1  # under pressure, so the mint pays

    rig.steps(2)

    (mint,) = [row for row in rig.store.broadcasts(None) if row.kind == store.MINT]
    (locked,) = rig.events("collateral_locked")
    assert mint.fee > 0
    assert locked.detail["fee"] == mint.fee


def test_a_confirmed_withdrawal_is_booked_with_its_fee() -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    rig.spot = 1.25  # the call's ask is now far too cheap

    rig.steps(1)
    assert rig.events("offers_withdrawn") == []  # sent, not yet confirmed
    rig.spot = 1.40
    rig.steps(2)

    (withdrawn,) = rig.events("offers_withdrawn")
    (sent,) = [row for row in rig.store.broadcasts(None) if row.kind == store.WITHDRAW]
    assert withdrawn.launcher_id == call.launcher_id
    assert withdrawn.detail == {"fee": sent.fee}


def test_a_sale_can_be_traced_to_the_quote_it_was_sold_at() -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    offer = rig.live_offer(call)

    rig.world.take(rig.taker, offer.offer_text)
    rig.steps(2)

    (sold,) = rig.events("sold")
    quote = rig.store.quotes()[sold.detail["offer_id"]]
    assert quote.launcher_id == call.launcher_id
    assert quote.units == sold.detail["amount"] == offer.price_amount


def test_what_a_step_reports_is_logged_marked_as_reported(caplog) -> None:
    rig = Rig()

    with caplog.at_level(logging.INFO, logger="gui.services.options.service"):
        reports = rig.steps(2)

    reported = [
        record.getMessage() for record in caplog.records if getattr(record, "options_report", False)
    ]
    actions = [action for report in reports for action in report.actions]
    assert actions and all(f"[options] {action}" in reported for action in actions)
    assert "[options] market: spot 1.1000 sigma 0.86 dexie 1.0780/1.1220" in reported


def test_a_step_that_fails_is_journaled_and_still_fails(monkeypatch) -> None:
    rig = Rig()
    rig.steps(1)

    def unreachable(*args: Any, **kwargs: Any) -> None:
        raise chain.ChainError("the node did not answer")

    monkeypatch.setattr(rig.book, "_look", unreachable)
    with pytest.raises(chain.ChainError, match="did not answer"):
        rig.book.step()

    row = rig.store.last_step()
    assert (row.verdict, row.detail) == (STEP_FAILED, "ChainError: the node did not answer")
