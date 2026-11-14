"""The options job store: what is recorded, and that it survives a restart."""

from __future__ import annotations

import sqlite3

import pytest

from gui.services.options import store

LAUNCHER = b"\x11" * 32
OTHER = b"\x22" * 32
USD = b"\x07" * 32
ME = b"\x33" * 32


def _add(book: store.OptionStore, launcher: bytes = LAUNCHER, **overrides) -> int:
    values = {
        "launcher_id": launcher,
        "slot": "C110@1000",
        "side": "call",
        "underlying_asset": None,
        "underlying_amount": 20,
        "strike_asset": USD,
        "strike_amount": 24,
        "expiration": 1000,
        "creator_puzzle_hash": ME,
        "bundle": b"mint-bundle",
        "deadline": 500,
        "now": 100,
    }
    values.update(overrides)
    return book.add_contract(**values)


def test_a_contract_and_its_mint_are_recorded_together() -> None:
    book = store.OptionStore(":memory:")

    _add(book)

    contract = book.contract(LAUNCHER)
    assert contract.status == store.MINTING
    assert (contract.underlying_asset, contract.strike_asset) == (None, USD)
    assert contract.creator_puzzle_hash == ME
    (mint,) = book.broadcasts()
    assert (mint.kind, mint.bundle, mint.status, mint.deadline) == (
        store.MINT,
        b"mint-bundle",
        "pending",
        500,
    )


def test_a_duplicate_contract_rolls_back_whole() -> None:
    book = store.OptionStore(":memory:")
    _add(book)
    with pytest.raises(sqlite3.IntegrityError):
        _add(book)
    assert len(book.contracts()) == 1
    assert len(book.broadcasts()) == 1  # no orphan broadcast from the failed insert


def test_status_change_and_event_are_one_transaction() -> None:
    book = store.OptionStore(":memory:")
    _add(book)

    book.set_status(LAUNCHER, store.OPEN, event="collateral_locked", detail={"amount": 20}, now=150)

    assert book.contract(LAUNCHER).status == store.OPEN
    assert book.contract(LAUNCHER).updated_at == 150
    (event,) = book.events(LAUNCHER)
    assert (event.kind, event.detail, event.at) == ("collateral_locked", {"amount": 20}, 150)


def test_contracts_filter_by_status() -> None:
    book = store.OptionStore(":memory:")
    _add(book)
    _add(book, OTHER, slot="P90@1000", side="put")
    book.set_status(OTHER, store.SOLD)

    assert [item.launcher_id for item in book.contracts((store.SOLD,))] == [OTHER]
    assert len(book.contracts(store.ACTIVE)) == 2
    assert len(book.contracts()) == 2
    assert book.contract(b"\x99" * 32) is None


def test_offers_live_and_die() -> None:
    book = store.OptionStore(":memory:")
    _add(book)
    first = book.add_offer(
        launcher_id=LAUNCHER,
        price_asset=USD,
        price_amount=900,
        expires_at=400,
        bundle=b"b1",
        offer_text="offer1a",
        now=100,
    )
    second = book.add_offer(
        launcher_id=LAUNCHER,
        price_asset=None,
        price_amount=950,
        expires_at=None,
        bundle=b"b2",
        offer_text="offer1b",
        now=110,
    )

    book.set_offer(first, desk_offer_id="abc")
    book.set_offer(second, status="taken")
    book.kill_offers(LAUNCHER)

    rows = book.offers(LAUNCHER)
    assert [(row.id, row.status, row.desk_offer_id) for row in rows] == [
        (first, "dead", "abc"),
        (second, "taken", None),  # a taken offer is not overwritten
    ]
    assert rows[1].price_asset is None and rows[1].expires_at is None
    assert book.offers(status="live") == []


def test_a_broadcast_can_move_its_contract_in_the_same_write() -> None:
    book = store.OptionStore(":memory:")
    _add(book)
    book.set_status(LAUNCHER, store.OPEN)

    row = book.add_broadcast(
        launcher_id=LAUNCHER,
        kind=store.CANCEL,
        bundle=b"cancel",
        deadline=900,
        contract_status=store.CLOSING,
        now=200,
    )
    book.set_broadcast(row, "confirmed")

    assert book.contract(LAUNCHER).status == store.CLOSING
    assert [item.kind for item in book.broadcasts()] == [store.MINT]
    assert [item.status for item in book.broadcasts(None)] == ["pending", "confirmed"]


def test_an_offer_for_an_unknown_contract_is_refused() -> None:
    book = store.OptionStore(":memory:")
    with pytest.raises(sqlite3.IntegrityError):
        book.add_offer(
            launcher_id=LAUNCHER,
            price_asset=USD,
            price_amount=1,
            expires_at=None,
            bundle=b"",
            offer_text="offer1",
        )


def test_everything_survives_a_restart(tmp_path) -> None:
    path = tmp_path / "nested" / "options_jobs.db"
    book = store.OptionStore(path)
    _add(book)
    book.add_event("note", {"text": "hello"})
    book.close()

    reopened = store.OptionStore(path)

    assert reopened.contract(LAUNCHER).slot == "C110@1000"
    assert len(reopened.broadcasts()) == 1
    assert reopened.events()[0].detail == {"text": "hello"}
