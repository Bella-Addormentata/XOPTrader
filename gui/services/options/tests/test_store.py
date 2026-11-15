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


def test_a_broadcast_remembers_the_time_it_is_valid_until() -> None:
    book = store.OptionStore(":memory:")
    _add(book, valid_until=700)
    book.set_status(LAUNCHER, store.OPEN)
    book.add_broadcast(
        launcher_id=LAUNCHER, kind=store.CANCEL, bundle=b"c", deadline=900, valid_until=950, now=200
    )
    book.add_broadcast(
        launcher_id=LAUNCHER, kind=store.CLAWBACK, bundle=b"k", deadline=900, now=210
    )

    assert [(row.kind, row.valid_until) for row in book.broadcasts(None)] == [
        (store.MINT, 700),
        (store.CANCEL, 950),
        (store.CLAWBACK, None),  # set by a caller with no chain clock to see it pass
    ]


def test_a_broadcast_remembers_what_it_paid() -> None:
    book = store.OptionStore(":memory:")
    _add(book, fee=1234)
    book.add_broadcast(launcher_id=LAUNCHER, kind=store.CANCEL, bundle=b"c", deadline=9, fee=5)
    book.add_broadcast(launcher_id=LAUNCHER, kind=store.WITHDRAW, bundle=b"w", deadline=9)

    assert [row.fee for row in book.broadcasts(None)] == [1234, 5, None]


def test_the_book_can_keep_small_things_it_has_learned(tmp_path) -> None:
    path = tmp_path / "options_jobs.db"
    book = store.OptionStore(path)
    assert book.recall("fee_rate") is None
    book.remember("fee_rate", "5.5")
    book.remember("fee_rate", "11.0")
    book.close()

    assert store.OptionStore(path).recall("fee_rate") == "11.0"


def test_a_database_from_before_deadlines_is_brought_up_to_date(tmp_path) -> None:
    path = tmp_path / "options_jobs.db"
    old = sqlite3.connect(path)
    old.executescript(
        """
        CREATE TABLE contracts (launcher_id TEXT PRIMARY KEY, slot TEXT NOT NULL,
            side TEXT NOT NULL, underlying_asset TEXT, underlying_amount INTEGER NOT NULL,
            strike_asset TEXT, strike_amount INTEGER NOT NULL, expiration INTEGER NOT NULL,
            creator_puzzle_hash TEXT NOT NULL, status TEXT NOT NULL,
            created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
        CREATE TABLE broadcasts (id INTEGER PRIMARY KEY AUTOINCREMENT,
            launcher_id TEXT NOT NULL REFERENCES contracts (launcher_id), kind TEXT NOT NULL,
            bundle BLOB NOT NULL, status TEXT NOT NULL, created_at INTEGER NOT NULL,
            deadline INTEGER NOT NULL);
        """
    )
    old.execute(
        "INSERT INTO contracts VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        (
            LAUNCHER.hex(),
            "C110@1000",
            "call",
            None,
            20,
            USD.hex(),
            24,
            1000,
            ME.hex(),
            "minting",
            1,
            1,
        ),
    )
    old.execute(
        "INSERT INTO broadcasts (launcher_id, kind, bundle, status, created_at, deadline)"
        " VALUES (?,?,?,?,?,?)",
        (LAUNCHER.hex(), "mint", b"old", "pending", 1, 500),
    )
    old.commit()
    old.close()

    book = store.OptionStore(path)

    (row,) = book.unresolved()
    # An old bundle has no deadline and no record of its fee.
    assert (row.bundle, row.valid_until, row.fee) == (b"old", None, None)
    book.add_broadcast(
        launcher_id=LAUNCHER, kind=store.CANCEL, bundle=b"new", deadline=900, valid_until=950
    )
    assert book.unresolved()[-1].valid_until == 950
    store.OptionStore(path).close()  # opening it again changes nothing


def test_withdrawals_are_counted_by_when_they_were_sent() -> None:
    book = store.OptionStore(":memory:")
    _add(book)
    _add(book, OTHER)
    for launcher, at in ((LAUNCHER, 1000), (LAUNCHER, 3000), (OTHER, 5000)):
        book.add_broadcast(
            launcher_id=launcher, kind=store.WITHDRAW, bundle=b"w", deadline=at + 600, now=at
        )
    book.add_broadcast(launcher_id=OTHER, kind=store.CANCEL, bundle=b"c", deadline=9000, now=5500)

    assert book.withdrawals() == {LAUNCHER: 3000, OTHER: 5000}
    assert [book.withdrawals_since(at) for at in (0, 3000, 3001, 6000)] == [3, 2, 1, 0]


def test_a_failed_mint_s_record_is_taken_over_when_the_same_coin_is_minted_from_again() -> None:
    book = store.OptionStore(":memory:")
    first = _add(book)
    book.settle_broadcast(first, "conflict", contract_status=store.FAILED, event="mint_failed")

    # Same funding coin, so the same launcher id; new terms.
    second = _add(book, slot="C120@2000", strike_amount=26, expiration=2000, now=900)

    contract = book.contract(LAUNCHER)
    assert (contract.status, contract.slot, contract.strike_amount, contract.expiration) == (
        store.MINTING,
        "C120@2000",
        26,
        2000,
    )
    assert contract.created_at == 900
    assert [(row.id, row.status) for row in book.broadcasts(None)] == [
        (first, "conflict"),
        (second, "pending"),
    ]
    assert [event.kind for event in book.events(LAUNCHER)] == ["mint_failed"]
    # A contract that is not a failed mint is never overwritten.
    with pytest.raises(sqlite3.IntegrityError):
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


def test_an_offer_records_when_a_splash_daemon_took_it() -> None:
    book = store.OptionStore(":memory:")
    _add(book)
    offer_id = _offer(book, 900)
    assert book.offers()[0].splash_at is None

    book.set_offer(offer_id, splash_at=1234)

    (row,) = book.offers()
    assert row.splash_at == 1234 and row.status == "live"


def test_a_database_from_before_splash_is_brought_up_to_date(tmp_path) -> None:
    path = tmp_path / "options_jobs.db"
    book = store.OptionStore(path)
    _add(book)
    _offer(book, 900)
    book.close()
    # Put the offers table back as it was before the column existed.
    old = sqlite3.connect(path)
    old.executescript(
        """
        ALTER TABLE offers RENAME TO offers_new;
        CREATE TABLE offers (id INTEGER PRIMARY KEY AUTOINCREMENT,
            launcher_id TEXT NOT NULL REFERENCES contracts (launcher_id), price_asset TEXT,
            price_amount INTEGER NOT NULL, expires_at INTEGER, bundle BLOB NOT NULL,
            offer_text TEXT NOT NULL, desk_offer_id TEXT, status TEXT NOT NULL,
            created_at INTEGER NOT NULL);
        INSERT INTO offers SELECT id, launcher_id, price_asset, price_amount, expires_at,
            bundle, offer_text, desk_offer_id, status, created_at FROM offers_new;
        DROP TABLE offers_new;
        """
    )
    old.commit()
    old.close()

    book = store.OptionStore(path)

    (row,) = book.offers()
    assert (row.price_amount, row.splash_at) == (900, None)
    book.set_offer(row.id, splash_at=77)
    assert book.offers()[0].splash_at == 77
