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


def _offer(book: store.OptionStore, price: int) -> int:
    return book.add_offer(
        launcher_id=LAUNCHER,
        price_asset=USD,
        price_amount=price,
        expires_at=None,
        bundle=b"offer",
        offer_text=f"offer{price}",
        now=150,
    )


def test_a_verdict_and_what_follows_from_it_are_one_write() -> None:
    book = store.OptionStore(":memory:")
    mint = _add(book)
    live = _offer(book, 5)

    book.settle_broadcast(
        mint,
        "confirmed",
        contract_status=store.CLOSED,
        kill_offers=True,
        event="collateral_returned",
        detail={"by": "cancel"},
        now=300,
    )

    assert book.broadcasts(None)[0].status == "confirmed"
    assert book.contract(LAUNCHER).status == store.CLOSED
    assert book.offers()[0].id == live and book.offers()[0].status == "dead"
    assert [(event.kind, event.detail) for event in book.events()] == [
        ("collateral_returned", {"by": "cancel"})
    ]


def test_a_verdict_that_cannot_be_written_whole_is_not_written_at_all() -> None:
    book = store.OptionStore(":memory:")
    mint = _add(book)
    _offer(book, 5)

    with pytest.raises(TypeError):  # the event detail cannot be stored
        book.settle_broadcast(
            mint,
            "confirmed",
            contract_status=store.OPEN,
            kill_offers=True,
            event="collateral_locked",
            detail={"amount": object()},
            now=300,
        )

    # Still unresolved, so the next step reads the verdict again.
    assert [row.id for row in book.unresolved()] == [mint]
    assert book.contract(LAUNCHER).status == store.MINTING
    assert book.offers()[0].status == "live"
    assert book.events() == []
    with pytest.raises(KeyError):
        book.settle_broadcast(999, "confirmed")


def test_a_verdict_can_be_made_to_move_a_contract_only_from_one_status() -> None:
    book = store.OptionStore(":memory:")
    _add(book)
    book.set_status(LAUNCHER, store.CLOSED)
    cancel = book.add_broadcast(
        launcher_id=LAUNCHER, kind=store.CANCEL, bundle=b"cancel", deadline=900, now=200
    )

    book.settle_broadcast(cancel, "conflict", contract_status=store.OPEN, only_from=store.CLOSING)

    assert book.contract(LAUNCHER).status == store.CLOSED  # it was not closing; left alone
    assert book.broadcasts("conflict")[0].id == cancel


def test_a_sale_marks_its_offer_and_kills_the_rest_in_one_write() -> None:
    book = store.OptionStore(":memory:")
    _add(book)
    old, taken, newer = _offer(book, 5), _offer(book, 6), _offer(book, 7)
    book.set_offer(old, status="dead")

    book.set_status(
        LAUNCHER,
        store.SOLD,
        event="sold",
        detail={"amount": 6},
        taken_offer=taken,
        kill_offers=True,
        now=400,
    )

    assert {row.id: row.status for row in book.offers()} == {
        old: "dead",
        taken: "taken",
        newer: "dead",
    }
    assert book.contract(LAUNCHER).status == store.SOLD
    assert book.events()[0].detail == {"amount": 6}


def test_stalled_broadcasts_are_still_unresolved() -> None:
    book = store.OptionStore(":memory:")
    mint = _add(book)
    _add(book, OTHER)
    book.set_broadcast(mint, "stalled")

    assert [row.status for row in book.unresolved()] == ["stalled", "pending"]
    book.settle_broadcast(mint, "confirmed")
    assert [row.status for row in book.unresolved()] == ["pending"]


def test_only_one_process_acts_on_a_database(tmp_path) -> None:
    path = tmp_path / "options_jobs.db"
    first, second = store.OptionStore(path), store.OptionStore(path)

    with first.acting():
        with first.acting():  # the holder may take it again
            pass
        with pytest.raises(store.BusyError, match="another process"), second.acting():
            pytest.fail("two processes held the acting lock")
        # Reading and writing rows is not what the lock guards.
        _add(second)
        assert first.contract(LAUNCHER) is not None

    with second.acting():  # released: the other may act now
        with pytest.raises(store.BusyError), first.acting():
            pytest.fail("two processes held the acting lock")


def _fail_while_acting(book: store.OptionStore) -> None:
    with book.acting():
        raise RuntimeError("boom")


def test_the_acting_lock_is_released_when_its_holder_fails_or_closes(tmp_path) -> None:
    path = tmp_path / "options_jobs.db"
    first, second = store.OptionStore(path), store.OptionStore(path)

    with pytest.raises(RuntimeError, match="boom"):
        _fail_while_acting(first)
    with second.acting():
        pass

    held = first.acting()
    held.__enter__()
    first.close()  # as a dying process would
    with second.acting():
        pass


def test_an_in_memory_database_needs_no_lock() -> None:
    book = store.OptionStore(":memory:")
    with book.acting(), book.acting():
        _add(book)


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
