"""Durable record of the options book: contracts, offers, broadcasts, events.

The reference wallet does not know an option exists, so this database is the
only local record of what was minted, what is on offer and what has been sent
to the chain. It lives in its own file (``data/options_jobs.db``), as warp's job
store does, and the engine never writes to it.

PERSIST, THEN ACT. Anything that changes the chain is written here before it is
attempted (:meth:`OptionStore.add_broadcast` stores the signed bundle before it
is pushed). A crash between the two leaves a row whose outcome can be read from
the chain on the next start; the reverse order would leave collateral locked in
a contract nothing remembers.

ONE VERDICT, ONE WRITE. What follows from a chain verdict (the broadcast's
status, the contract's, its offers', the event) is committed together
(:meth:`OptionStore.settle_broadcast`, :meth:`OptionStore.set_status`). A crash
can leave a verdict unread; it cannot leave one half applied.

ONE PROCESS ACTS. :meth:`OptionStore.acting` is a lock on the database file.
Whoever signs or spends holds it, so two processes over one database cannot
both plan from the same rows and mint the same slot twice.

The ``events`` table is an append-only log of what happened to our money (a
mint locked collateral, a premium arrived, a contract was exercised). It is
what an accounting integration would read; nothing reads it yet.

``chain_facts`` is not a record of ours at all. It keeps what was read from the
chain about spent coins (that a coin was spent, at what height, and how), so
that it is not asked for twice, across restarts included. A row starts out
provisional and becomes ``final`` once the spend is old enough that a reorg
cannot undo it; only final rows are trusted without looking again. Deleting the
table loses nothing but the lookups it saves.
"""

from __future__ import annotations

import contextlib
import dataclasses
import json
import sqlite3
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any

# Contract lifecycle.
MINTING = "minting"  # mint broadcast, not yet confirmed
OPEN = "open"  # minted and still ours: can be offered or cancelled
SOLD = "sold"  # somebody else owns the option; collateral stays locked
CLOSING = "closing"  # a cancel or clawback is broadcast
CLOSED = "closed"  # collateral is back with us (cancelled or clawed back)
EXERCISED = "exercised"  # the holder took the collateral and paid the strike
FAILED = "failed"  # the mint never confirmed; nothing is locked

ACTIVE = (MINTING, OPEN, SOLD, CLOSING)

# Broadcast kinds.
MINT = "mint"
CANCEL = "cancel"
CLAWBACK = "clawback"
WITHDRAW = "withdraw"  # transfer to self: kills every open offer for the coin

_SCHEMA = """
CREATE TABLE IF NOT EXISTS contracts (
    launcher_id        TEXT PRIMARY KEY,
    slot               TEXT NOT NULL,
    side               TEXT NOT NULL CHECK (side IN ('call', 'put')),
    underlying_asset   TEXT,
    underlying_amount  INTEGER NOT NULL,
    strike_asset       TEXT,
    strike_amount      INTEGER NOT NULL,
    expiration         INTEGER NOT NULL,
    creator_puzzle_hash TEXT NOT NULL,
    status             TEXT NOT NULL,
    created_at         INTEGER NOT NULL,
    updated_at         INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_contracts_status ON contracts (status);

CREATE TABLE IF NOT EXISTS offers (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    launcher_id   TEXT NOT NULL REFERENCES contracts (launcher_id),
    price_asset   TEXT,
    price_amount  INTEGER NOT NULL,
    expires_at    INTEGER,
    bundle        BLOB NOT NULL,
    offer_text    TEXT NOT NULL,
    desk_offer_id TEXT,
    status        TEXT NOT NULL CHECK (status IN ('live', 'taken', 'dead')),
    created_at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_offers_launcher ON offers (launcher_id, status);

CREATE TABLE IF NOT EXISTS broadcasts (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    launcher_id TEXT NOT NULL REFERENCES contracts (launcher_id),
    kind        TEXT NOT NULL,
    bundle      BLOB NOT NULL,
    status      TEXT NOT NULL CHECK (status IN ('pending', 'confirmed', 'conflict', 'stalled')),
    created_at  INTEGER NOT NULL,
    deadline    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS ix_broadcasts_status ON broadcasts (status);

CREATE TABLE IF NOT EXISTS listings (
    offer_id      TEXT PRIMARY KEY,
    launcher_id   TEXT NOT NULL,
    side          TEXT,
    size_xch      REAL,
    strike        REAL,
    expiration    INTEGER,
    ask           REAL,
    offer_expires INTEGER,
    site_premium  REAL,
    our_value     REAL,
    hedged_value  REAL,
    bundle        BLOB NOT NULL,
    first_seen    INTEGER NOT NULL,
    last_seen     INTEGER NOT NULL,
    outcome       TEXT,
    closed_at     INTEGER
);
CREATE INDEX IF NOT EXISTS ix_listings_open ON listings (outcome);

CREATE TABLE IF NOT EXISTS chain_facts (
    coin_id        TEXT PRIMARY KEY,
    parent         TEXT NOT NULL,
    puzzle_hash    TEXT NOT NULL,
    amount         TEXT NOT NULL,
    created_height INTEGER,
    spent_height   INTEGER NOT NULL,
    puzzle_reveal  BLOB,
    solution       BLOB,
    seen_at        INTEGER NOT NULL,
    final          INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    at          INTEGER NOT NULL,
    launcher_id TEXT,
    kind        TEXT NOT NULL,
    detail      TEXT NOT NULL
);
"""


class BusyError(RuntimeError):
    """Another process is acting on this database."""


def _hex(value: bytes | None) -> str | None:
    return None if value is None else bytes(value).hex()


def _unhex(value: str | None) -> bytes | None:
    return None if value is None else bytes.fromhex(value)


@dataclasses.dataclass(frozen=True)
class Contract:
    launcher_id: bytes
    slot: str
    side: str
    underlying_asset: bytes | None
    underlying_amount: int
    strike_asset: bytes | None
    strike_amount: int
    expiration: int
    creator_puzzle_hash: bytes
    status: str
    created_at: int
    updated_at: int


@dataclasses.dataclass(frozen=True)
class OfferRow:
    id: int
    launcher_id: bytes
    price_asset: bytes | None
    price_amount: int
    expires_at: int | None
    bundle: bytes
    offer_text: str
    desk_offer_id: str | None
    status: str
    created_at: int


@dataclasses.dataclass(frozen=True)
class Broadcast:
    id: int
    launcher_id: bytes
    kind: str
    bundle: bytes
    status: str
    created_at: int
    deadline: int


@dataclasses.dataclass(frozen=True)
class Event:
    id: int
    at: int
    launcher_id: bytes | None
    kind: str
    detail: dict


@dataclasses.dataclass(frozen=True)
class Listing:
    """Somebody's offer as the desk monitor saw it. Prices are contract totals."""

    offer_id: str
    launcher_id: bytes
    side: str | None  # None when the legs are not XCH against the quote asset
    size_xch: float | None
    strike: float | None
    expiration: int | None
    ask: float | None
    offer_expires: int | None
    site_premium: float | None
    our_value: float | None
    hedged_value: float | None  # our value at the Dexie price a hedge would get
    bundle: bytes
    first_seen: int
    last_seen: int
    outcome: str | None  # taken | withdrawn | expired, once it has left the desk
    closed_at: int | None

    @property
    def edge(self) -> float | None:
        """What taking it would be worth after hedging, in the quote asset."""
        if self.hedged_value is None or self.ask is None:
            return None
        return self.hedged_value - self.ask


@dataclasses.dataclass(frozen=True)
class ChainFact:
    """A spent coin as it was read from the chain.

    ``seen_at`` is when it was first seen spent at ``spent_height``. ``final``
    is set once it has stayed that way long enough to be beyond a reorg.
    ``puzzle_reveal`` and ``solution`` are None until the spend itself is read.
    """

    coin_id: bytes
    parent: bytes
    puzzle_hash: bytes
    amount: int
    created_height: int | None
    spent_height: int
    seen_at: int
    final: bool = False
    puzzle_reveal: bytes | None = None
    solution: bytes | None = None


class OptionStore:
    def __init__(self, path: str | Path) -> None:
        self._path = str(path)
        if self._path != ":memory:":
            Path(self._path).parent.mkdir(parents=True, exist_ok=True)
        self._db = sqlite3.connect(self._path)
        self._db.row_factory = sqlite3.Row
        self._db.execute("PRAGMA foreign_keys = ON")
        with self._db:
            self._db.executescript(_SCHEMA)
        self._lock: sqlite3.Connection | None = None
        self._lock_depth = 0

    def close(self) -> None:
        if self._lock is not None:
            self._lock.close()
            self._lock = None
            self._lock_depth = 0
        self._db.close()

    @contextlib.contextmanager
    def _write(self) -> Iterator[sqlite3.Connection]:
        with self._db:  # one transaction: commit on success, roll back on error
            yield self._db

    @contextlib.contextmanager
    def acting(self) -> Iterator[None]:
        """Hold this database's acting lock, or raise :class:`BusyError`.

        The lock is an exclusive transaction on a second file beside the
        database (``<path>.lock``), so the operating system releases it if the
        process dies, and it behaves the same on every platform SQLite does. It
        never waits: a second process is told at once. One store may take it
        again while it holds it. An in-memory database has no other process to
        exclude.
        """
        if self._path == ":memory:":
            yield
            return
        if self._lock is None:
            lock = sqlite3.connect(self._path + ".lock", timeout=0, isolation_level=None)
            try:
                lock.execute("BEGIN EXCLUSIVE")
            except sqlite3.OperationalError as exc:
                lock.close()
                raise BusyError(
                    "another process is acting on this options database; it was left alone"
                ) from exc
            self._lock = lock
        self._lock_depth += 1
        try:
            yield
        finally:
            self._lock_depth -= 1
            if self._lock_depth == 0 and self._lock is not None:
                self._lock.close()  # ends the transaction and frees the file
                self._lock = None

    # -- contracts ----------------------------------------------------------

    def add_contract(
        self,
        *,
        launcher_id: bytes,
        slot: str,
        side: str,
        underlying_asset: bytes | None,
        underlying_amount: int,
        strike_asset: bytes | None,
        strike_amount: int,
        expiration: int,
        creator_puzzle_hash: bytes,
        bundle: bytes,
        deadline: int,
        now: int | None = None,
    ) -> int:
        """Record a contract and its mint broadcast in one transaction."""
        now = int(time.time()) if now is None else int(now)
        with self._write() as db:
            db.execute(
                "INSERT INTO contracts VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    _hex(launcher_id),
                    slot,
                    side,
                    _hex(underlying_asset),
                    int(underlying_amount),
                    _hex(strike_asset),
                    int(strike_amount),
                    int(expiration),
                    _hex(creator_puzzle_hash),
                    MINTING,
                    now,
                    now,
                ),
            )
            cursor = db.execute(
                "INSERT INTO broadcasts (launcher_id, kind, bundle, status, created_at, deadline)"
                " VALUES (?,?,?,?,?,?)",
                (_hex(launcher_id), MINT, bytes(bundle), "pending", now, int(deadline)),
            )
            return int(cursor.lastrowid or 0)

    @staticmethod
    def _contract(row: sqlite3.Row) -> Contract:
        return Contract(
            launcher_id=bytes.fromhex(row["launcher_id"]),
            slot=row["slot"],
            side=row["side"],
            underlying_asset=_unhex(row["underlying_asset"]),
            underlying_amount=int(row["underlying_amount"]),
            strike_asset=_unhex(row["strike_asset"]),
            strike_amount=int(row["strike_amount"]),
            expiration=int(row["expiration"]),
            creator_puzzle_hash=bytes.fromhex(row["creator_puzzle_hash"]),
            status=row["status"],
            created_at=int(row["created_at"]),
            updated_at=int(row["updated_at"]),
        )

    def contract(self, launcher_id: bytes) -> Contract | None:
        row = self._db.execute(
            "SELECT * FROM contracts WHERE launcher_id = ?", (_hex(launcher_id),)
        ).fetchone()
        return None if row is None else self._contract(row)

    def contracts(self, statuses: tuple[str, ...] | None = None) -> list[Contract]:
        if statuses is None:
            rows = self._db.execute("SELECT * FROM contracts ORDER BY created_at, launcher_id")
        else:
            marks = ",".join("?" for _ in statuses)
            rows = self._db.execute(
                f"SELECT * FROM contracts WHERE status IN ({marks})"  # noqa: S608 - marks only
                " ORDER BY created_at, launcher_id",
                statuses,
            )
        return [self._contract(row) for row in rows]

    @staticmethod
    def _transition(
        db: sqlite3.Connection,
        launcher: str,
        *,
        status: str | None,
        only_from: str | None,
        taken_offer: int | None,
        kill_offers: bool,
        event: str | None,
        detail: dict | None,
        now: int,
    ) -> None:
        """Apply one contract transition inside the caller's transaction."""
        if taken_offer is not None:
            db.execute("UPDATE offers SET status = 'taken' WHERE id = ?", (int(taken_offer),))
        if kill_offers:
            db.execute(
                "UPDATE offers SET status = 'dead' WHERE launcher_id = ? AND status = 'live'",
                (launcher,),
            )
        if status is not None and only_from is None:
            db.execute(
                "UPDATE contracts SET status = ?, updated_at = ? WHERE launcher_id = ?",
                (status, now, launcher),
            )
        elif status is not None:
            db.execute(
                "UPDATE contracts SET status = ?, updated_at = ? WHERE launcher_id = ?"
                " AND status = ?",
                (status, now, launcher, only_from),
            )
        if event is not None:
            db.execute(
                "INSERT INTO events (at, launcher_id, kind, detail) VALUES (?,?,?,?)",
                (now, launcher, event, json.dumps(detail or {}, sort_keys=True)),
            )

    def set_status(
        self,
        launcher_id: bytes,
        status: str,
        *,
        event: str | None = None,
        detail: dict | None = None,
        taken_offer: int | None = None,
        kill_offers: bool = False,
        now: int | None = None,
    ) -> None:
        """Move a contract to ``status`` and record what came with it, in one write.

        ``taken_offer`` marks that offer taken, ``kill_offers`` marks every
        offer still live dead, and ``event`` is logged. Either all of it is
        committed or none of it.
        """
        now = int(time.time()) if now is None else int(now)
        with self._write() as db:
            self._transition(
                db,
                _hex(launcher_id) or "",
                status=status,
                only_from=None,
                taken_offer=taken_offer,
                kill_offers=kill_offers,
                event=event,
                detail=detail,
                now=now,
            )

    # -- offers -------------------------------------------------------------

    def add_offer(
        self,
        *,
        launcher_id: bytes,
        price_asset: bytes | None,
        price_amount: int,
        expires_at: int | None,
        bundle: bytes,
        offer_text: str,
        now: int | None = None,
    ) -> int:
        now = int(time.time()) if now is None else int(now)
        with self._write() as db:
            cursor = db.execute(
                "INSERT INTO offers (launcher_id, price_asset, price_amount, expires_at, bundle,"
                " offer_text, desk_offer_id, status, created_at) VALUES (?,?,?,?,?,?,?,?,?)",
                (
                    _hex(launcher_id),
                    _hex(price_asset),
                    int(price_amount),
                    None if expires_at is None else int(expires_at),
                    bytes(bundle),
                    offer_text,
                    None,
                    "live",
                    now,
                ),
            )
            return int(cursor.lastrowid or 0)

    @staticmethod
    def _offer(row: sqlite3.Row) -> OfferRow:
        return OfferRow(
            id=int(row["id"]),
            launcher_id=bytes.fromhex(row["launcher_id"]),
            price_asset=_unhex(row["price_asset"]),
            price_amount=int(row["price_amount"]),
            expires_at=None if row["expires_at"] is None else int(row["expires_at"]),
            bundle=bytes(row["bundle"]),
            offer_text=row["offer_text"],
            desk_offer_id=row["desk_offer_id"],
            status=row["status"],
            created_at=int(row["created_at"]),
        )

    def offers(self, launcher_id: bytes | None = None, status: str | None = None) -> list[OfferRow]:
        clauses, values = [], []
        if launcher_id is not None:
            clauses.append("launcher_id = ?")
            values.append(_hex(launcher_id))
        if status is not None:
            clauses.append("status = ?")
            values.append(status)
        where = (" WHERE " + " AND ".join(clauses)) if clauses else ""
        rows = self._db.execute(
            f"SELECT * FROM offers{where} ORDER BY id",  # noqa: S608 - fixed clauses only
            values,
        )
        return [self._offer(row) for row in rows]

    def set_offer(
        self, offer_id: int, *, status: str | None = None, desk_offer_id: str | None = None
    ) -> None:
        with self._write() as db:
            if status is not None:
                db.execute("UPDATE offers SET status = ? WHERE id = ?", (status, int(offer_id)))
            if desk_offer_id is not None:
                db.execute(
                    "UPDATE offers SET desk_offer_id = ? WHERE id = ?",
                    (desk_offer_id, int(offer_id)),
                )

    def kill_offers(self, launcher_id: bytes) -> None:
        """Mark every live offer for a contract dead (its coin was spent)."""
        with self._write() as db:
            db.execute(
                "UPDATE offers SET status = 'dead' WHERE launcher_id = ? AND status = 'live'",
                (_hex(launcher_id),),
            )

    # -- broadcasts ---------------------------------------------------------

    def add_broadcast(
        self,
        *,
        launcher_id: bytes,
        kind: str,
        bundle: bytes,
        deadline: int,
        contract_status: str | None = None,
        now: int | None = None,
    ) -> int:
        """Record a signed bundle BEFORE it is pushed; optionally move the contract too."""
        now = int(time.time()) if now is None else int(now)
        with self._write() as db:
            cursor = db.execute(
                "INSERT INTO broadcasts (launcher_id, kind, bundle, status, created_at, deadline)"
                " VALUES (?,?,?,?,?,?)",
                (_hex(launcher_id), kind, bytes(bundle), "pending", now, int(deadline)),
            )
            if contract_status is not None:
                db.execute(
                    "UPDATE contracts SET status = ?, updated_at = ? WHERE launcher_id = ?",
                    (contract_status, now, _hex(launcher_id)),
                )
            return int(cursor.lastrowid or 0)

    def broadcasts(self, status: str | None = "pending") -> list[Broadcast]:
        if status is None:
            rows = self._db.execute("SELECT * FROM broadcasts ORDER BY id")
        else:
            rows = self._db.execute(
                "SELECT * FROM broadcasts WHERE status = ? ORDER BY id", (status,)
            )
        return [
            Broadcast(
                id=int(row["id"]),
                launcher_id=bytes.fromhex(row["launcher_id"]),
                kind=row["kind"],
                bundle=bytes(row["bundle"]),
                status=row["status"],
                created_at=int(row["created_at"]),
                deadline=int(row["deadline"]),
            )
            for row in rows
        ]

    def unresolved(self) -> list[Broadcast]:
        """Broadcasts the chain has not ruled on yet: pending, and flagged as stalled.

        A stalled bundle is still signed and can still confirm, so it is treated
        exactly as a pending one everywhere but in what the operator is told.
        """
        rows = self.broadcasts("pending") + self.broadcasts("stalled")
        return sorted(rows, key=lambda row: row.id)

    def set_broadcast(self, broadcast_id: int, status: str) -> None:
        with self._write() as db:
            db.execute("UPDATE broadcasts SET status = ? WHERE id = ?", (status, int(broadcast_id)))

    def settle_broadcast(
        self,
        broadcast_id: int,
        status: str,
        *,
        contract_status: str | None = None,
        only_from: str | None = None,
        kill_offers: bool = False,
        event: str | None = None,
        detail: dict | None = None,
        now: int | None = None,
    ) -> None:
        """Record a broadcast's verdict and all that follows from it, in one write.

        ``contract_status`` moves the broadcast's contract (only out of
        ``only_from`` when that is given), ``kill_offers`` marks its live offers
        dead and ``event`` is logged against it. A crash before the commit
        leaves the broadcast unresolved, so the next step reads the verdict
        again; there is no state in which the verdict is stored and the
        contract has not followed.
        """
        now = int(time.time()) if now is None else int(now)
        with self._write() as db:
            row = db.execute(
                "SELECT launcher_id FROM broadcasts WHERE id = ?", (int(broadcast_id),)
            ).fetchone()
            if row is None:
                raise KeyError(f"no broadcast {broadcast_id}")
            db.execute("UPDATE broadcasts SET status = ? WHERE id = ?", (status, int(broadcast_id)))
            self._transition(
                db,
                row["launcher_id"],
                status=contract_status,
                only_from=only_from,
                taken_offer=None,
                kill_offers=kill_offers,
                event=event,
                detail=detail,
                now=now,
            )

    # -- events -------------------------------------------------------------

    def add_event(
        self,
        kind: str,
        detail: dict[str, Any] | None = None,
        *,
        launcher_id: bytes | None = None,
        now: int | None = None,
    ) -> None:
        now = int(time.time()) if now is None else int(now)
        with self._write() as db:
            db.execute(
                "INSERT INTO events (at, launcher_id, kind, detail) VALUES (?,?,?,?)",
                (now, _hex(launcher_id), kind, json.dumps(detail or {}, sort_keys=True)),
            )

    def events(self, launcher_id: bytes | None = None) -> list[Event]:
        if launcher_id is None:
            rows = self._db.execute("SELECT * FROM events ORDER BY id")
        else:
            rows = self._db.execute(
                "SELECT * FROM events WHERE launcher_id = ? ORDER BY id", (_hex(launcher_id),)
            )
        return [
            Event(
                id=int(row["id"]),
                at=int(row["at"]),
                launcher_id=_unhex(row["launcher_id"]),
                kind=row["kind"],
                detail=json.loads(row["detail"]),
            )
            for row in rows
        ]

    # -- what was read from the chain (a cache, not a record of ours) --------

    def chain_fact(self, coin_id: bytes) -> ChainFact | None:
        row = self._db.execute(
            "SELECT * FROM chain_facts WHERE coin_id = ?", (_hex(coin_id),)
        ).fetchone()
        if row is None:
            return None
        reveal, solution = row["puzzle_reveal"], row["solution"]
        return ChainFact(
            coin_id=bytes.fromhex(row["coin_id"]),
            parent=bytes.fromhex(row["parent"]),
            puzzle_hash=bytes.fromhex(row["puzzle_hash"]),
            amount=int(row["amount"]),
            created_height=None if row["created_height"] is None else int(row["created_height"]),
            spent_height=int(row["spent_height"]),
            seen_at=int(row["seen_at"]),
            final=bool(row["final"]),
            puzzle_reveal=None if reveal is None else bytes(reveal),
            solution=None if solution is None else bytes(solution),
        )

    def remember_chain_fact(self, fact: ChainFact) -> None:
        """Keep what was read about a spent coin. Writing it again replaces it."""
        with self._write() as db:
            db.execute(
                "INSERT OR REPLACE INTO chain_facts VALUES (?,?,?,?,?,?,?,?,?,?)",
                (
                    _hex(fact.coin_id),
                    _hex(fact.parent),
                    _hex(fact.puzzle_hash),
                    str(int(fact.amount)),  # text: a coin can exceed SQLite's 63 bits
                    None if fact.created_height is None else int(fact.created_height),
                    int(fact.spent_height),
                    None if fact.puzzle_reveal is None else bytes(fact.puzzle_reveal),
                    None if fact.solution is None else bytes(fact.solution),
                    int(fact.seen_at),
                    1 if fact.final else 0,
                ),
            )

    # -- desk listings (other makers' offers, read-only monitoring) ---------

    def record_listing(self, listing: Listing) -> bool:
        """Insert a listing, or refresh ``last_seen`` if known. True if it was new."""
        with self._write() as db:
            known = db.execute(
                "SELECT 1 FROM listings WHERE offer_id = ?", (listing.offer_id,)
            ).fetchone()
            if known is not None:
                db.execute(
                    "UPDATE listings SET last_seen = ? WHERE offer_id = ?",
                    (int(listing.last_seen), listing.offer_id),
                )
                return False
            db.execute(
                "INSERT INTO listings VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    listing.offer_id,
                    _hex(listing.launcher_id),
                    listing.side,
                    listing.size_xch,
                    listing.strike,
                    listing.expiration,
                    listing.ask,
                    listing.offer_expires,
                    listing.site_premium,
                    listing.our_value,
                    listing.hedged_value,
                    bytes(listing.bundle),
                    int(listing.first_seen),
                    int(listing.last_seen),
                    listing.outcome,
                    listing.closed_at,
                ),
            )
            return True

    def listings(self, *, open_only: bool = False) -> list[Listing]:
        where = " WHERE outcome IS NULL" if open_only else ""
        rows = self._db.execute(
            f"SELECT * FROM listings{where} ORDER BY first_seen, offer_id"  # noqa: S608 - fixed
        )
        return [
            Listing(
                offer_id=row["offer_id"],
                launcher_id=bytes.fromhex(row["launcher_id"]),
                side=row["side"],
                size_xch=row["size_xch"],
                strike=row["strike"],
                expiration=row["expiration"],
                ask=row["ask"],
                offer_expires=row["offer_expires"],
                site_premium=row["site_premium"],
                our_value=row["our_value"],
                hedged_value=row["hedged_value"],
                bundle=bytes(row["bundle"]),
                first_seen=int(row["first_seen"]),
                last_seen=int(row["last_seen"]),
                outcome=row["outcome"],
                closed_at=row["closed_at"],
            )
            for row in rows
        ]

    def closed_listing_ids(self) -> set[str]:
        """Offer ids whose listing has already been closed with an outcome."""
        rows = self._db.execute("SELECT offer_id FROM listings WHERE outcome IS NOT NULL")
        return {row["offer_id"] for row in rows}

    def close_listing(self, offer_id: str, outcome: str, now: int) -> None:
        with self._write() as db:
            db.execute(
                "UPDATE listings SET outcome = ?, closed_at = ? WHERE offer_id = ?",
                (outcome, int(now), offer_id),
            )
