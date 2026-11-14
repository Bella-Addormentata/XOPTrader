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

The ``events`` table is an append-only log of what happened to our money (a
mint locked collateral, a premium arrived, a contract was exercised). It is
what an accounting integration would read; nothing reads it yet.
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

CREATE TABLE IF NOT EXISTS events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    at          INTEGER NOT NULL,
    launcher_id TEXT,
    kind        TEXT NOT NULL,
    detail      TEXT NOT NULL
);
"""


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

    def close(self) -> None:
        self._db.close()

    @contextlib.contextmanager
    def _write(self) -> Iterator[sqlite3.Connection]:
        with self._db:  # one transaction: commit on success, roll back on error
            yield self._db

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

    def set_status(
        self,
        launcher_id: bytes,
        status: str,
        *,
        event: str | None = None,
        detail: dict | None = None,
        now: int | None = None,
    ) -> None:
        """Move a contract to ``status``, logging ``event`` in the same transaction."""
        now = int(time.time()) if now is None else int(now)
        with self._write() as db:
            db.execute(
                "UPDATE contracts SET status = ?, updated_at = ? WHERE launcher_id = ?",
                (status, now, _hex(launcher_id)),
            )
            if event is not None:
                db.execute(
                    "INSERT INTO events (at, launcher_id, kind, detail) VALUES (?,?,?,?)",
                    (now, _hex(launcher_id), event, json.dumps(detail or {}, sort_keys=True)),
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

    def set_broadcast(self, broadcast_id: int, status: str) -> None:
        with self._write() as db:
            db.execute("UPDATE broadcasts SET status = ? WHERE id = ?", (status, int(broadcast_id)))

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

    def close_listing(self, offer_id: str, outcome: str, now: int) -> None:
        with self._write() as db:
            db.execute(
                "UPDATE listings SET outcome = ?, closed_at = ? WHERE offer_id = ?",
                (outcome, int(now), offer_id),
            )
